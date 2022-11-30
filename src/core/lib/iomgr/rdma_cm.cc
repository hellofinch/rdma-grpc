#include <grpc/support/port_platform.h>

// #ifdef GPR_POSIX_SOCKET

//#include "src/core/lib/iomgr/network_status_tracker.h"
#include "src/core/lib/iomgr/rdma_cm.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <fcntl.h>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/impl/codegen/gpr_slice.h>
#include <grpc/support/string_util.h>
#include <grpc/support/sync.h>
#include <grpc/support/time.h>

#include "src/core/lib/debug/trace.h"
#include "src/core/lib/iomgr/ev_posix.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/profiling/timers.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/slice/slice_string_helpers.h"


#include <stdio.h>
#include "src/core/lib/iomgr/rdma_utils_posix.h"

#ifdef GPR_HAVE_MSG_NOSIGNAL
#define SENDMSG_FLAGS MSG_NOSIGNAL
#else
#define SENDMSG_FLAGS 0
#endif

#ifdef GPR_MSG_IOVLEN_TYPE
typedef GPR_MSG_IOVLEN_TYPE msg_iovlen_type;`
#else
typedef size_t msg_iovlen_type;
#endif

int grpc_rdma_trace = 0;

typedef struct {
  grpc_endpoint base;
  connect_context* content;
  int fd;
  msg_iovlen_type iov_size; /* Number of slices to allocate per read attempt */
  size_t slice_size;
  gpr_refcount refcount;

  gpr_slice_buffer *incoming_buffer;
  gpr_slice_buffer temp_buffer;
  gpr_slice_buffer *outgoing_buffer;
  size_t outgoing_length;
  /** slice within outgoing_buffer to write next */
  size_t outgoing_slice_idx;
  /** byte within outgoing_buffer->slices[outgoing_slice_idx] to write next */
  size_t outgoing_byte_idx;

  grpc_closure *read_cb;
  grpc_closure *write_cb;
  grpc_closure *release_fd_cb;
  int *release_fd_in,*release_fd_out;

  grpc_closure read_closure;
  grpc_closure write_closure;

  std::string peer_string;
  bool dead,rflag,wflag,msg_pending;
  gpr_mu mu_death,mu_rflag,mu_wflag,mu_bufcount;
  int peer_buffer_count;
} grpc_rdma;
static void rdma_handle_read( void *arg /* grpc_rdma */,
                            grpc_error_handle error);
static void rdma_handle_write(void *arg /* grpc_rdma */,
                             grpc_error_handle error);
static void rdma_on_send_complete(grpc_rdma *rdma,grpc_error_handle error);
static void rdma_sentence_death(grpc_rdma*);
static bool rdma_flush(grpc_rdma *rdma, grpc_error_handle *error) ;
static void rdma_shutdown(grpc_endpoint *ep,grpc_error_handle /*error*/) {
  grpc_rdma *rdma = (grpc_rdma *)ep;
  if(rdma->dead) return;
  //gpr_log(GPR_DEBUG,"DESTROY:RDMA_CM_ID=%p",rdma->content->id);
  rdma_disconnect(rdma->content->id);
  //gpr_log(GPR_DEBUG,"ENDPOINT:DISCONNECTED");
  grpc_fd_shutdown(rdma->content->sendfdobj,GRPC_ERROR_CREATE_FROM_STATIC_STRING("rdma_shutdown send destroyed"));
  grpc_fd_shutdown(rdma->content->recvfdobj,GRPC_ERROR_CREATE_FROM_STATIC_STRING("rdma_shutdown recv destroyed"));
  rdma->msg_pending=false;
  //gpr_log(GPR_DEBUG,"ENDPOINT:FD CLOSED");
  //grpc_exec_ctx_flush(exec_ctx);
}

static void rdma_free(grpc_rdma *rdma) {
  gpr_slice_buffer_reset_and_unref(&rdma->temp_buffer);
  grpc_closure *clicb=rdma->content->closure;
  rdma_ctx_unref(rdma->content);
  if(clicb){
    //gpr_log(GPR_DEBUG,"Call callback of rdma_client_posix");
    clicb->cb(clicb->cb_arg,GRPC_ERROR_NONE);
  }
  gpr_free(rdma);
  gpr_log(GPR_DEBUG,"Endpoint:Goodbye~");
}
//#define GRPC_RDMA_REFCOUNT_DEBUG
#ifdef GRPC_RDMA_REFCOUNT_DEBUG
#define RDMA_UNREF(rdma, reason) \
  rdma_unref( (rdma), (reason), __FILE__, __LINE__)
#define RDMA_REF(rdma, reason) rdma_ref((rdma), (reason), __FILE__, __LINE__)
static void rdma_unref(grpc_rdma *rdma,
                      const char *reason, const char *file, int line) {
  gpr_log(file, line, GPR_LOG_SEVERITY_DEBUG, "TCP unref %p : %s %d -> %d", rdma,
          reason, (int)rdma->refcount.count, (int)rdma->refcount.count - 1);
  if (gpr_unref(&rdma->refcount)) {
    rdma_free( rdma);
  }
}

static void rdma_ref(grpc_rdma *rdma, const char *reason, const char *file,
                    int line) {
  gpr_log(file, line, GPR_LOG_SEVERITY_DEBUG, "TCP   ref %p : %s %d -> %d", rdma,
          reason, (int)rdma->refcount.count, (int)rdma->refcount.count + 1);
  gpr_ref(&rdma->refcount);
}
#else
#define RDMA_UNREF(rdma, reason) rdma_unref((rdma))
#define RDMA_REF(rdma, reason) rdma_ref((rdma))
static void rdma_unref(grpc_rdma *rdma) {
  if (gpr_unref(&rdma->refcount)) {
    rdma_free(rdma);
  }
}

static void rdma_ref(grpc_rdma *rdma) { 
  gpr_ref(&rdma->refcount);
}

#endif

static void rdma_destroy(grpc_endpoint *ep) {
  grpc_rdma *rdma = (grpc_rdma *)ep;
 // grpc_network_status_unregister_endpoint(ep);
  rdma_sentence_death(rdma);
  RDMA_UNREF( rdma, "destroy");
}
static void readfd_notify(grpc_rdma *rdma){
  if(rdma->rflag) return;
  gpr_mu_lock(&rdma->mu_rflag);
  grpc_fd_notify_on_read(rdma->content->recvfdobj, &rdma->read_closure);
  rdma->rflag=true;
  gpr_mu_unlock(&rdma->mu_rflag);
}
static void readfd_notified(grpc_rdma *rdma){
  gpr_mu_lock(&rdma->mu_rflag);
  rdma->rflag=false;
  gpr_mu_unlock(&rdma->mu_rflag);
}
static void writefd_notify(grpc_rdma *rdma){
  if(rdma->wflag) return;
  gpr_mu_lock(&rdma->mu_wflag);
  grpc_fd_notify_on_read(rdma->content->sendfdobj, &rdma->write_closure);
  rdma->wflag=true;
  gpr_mu_unlock(&rdma->mu_wflag);
}
static void writefd_notified(grpc_rdma *rdma){
  gpr_mu_lock(&rdma->mu_wflag);
  rdma->wflag=false;
  gpr_mu_unlock(&rdma->mu_wflag);
}
static void call_read_cb(grpc_rdma *rdma,
                         grpc_error_handle error) {
  grpc_closure *cb = rdma->read_cb;
  if(!cb) return;
  //gpr_log(GPR_DEBUG,"Called_READCB");
  if (grpc_rdma_trace) {
    size_t i;
    gpr_log(GPR_DEBUG, "read: error=%s", grpc_error_std_string(error).c_str());
    for (i = 0; i < rdma->incoming_buffer->count; i++) {
      char *dump = grpc_dump_slice(rdma->incoming_buffer->slices[i],
                                  GPR_DUMP_HEX | GPR_DUMP_ASCII);
      gpr_log(GPR_DEBUG, "READ %p (peer=%s): %s", rdma, rdma->peer_string, dump);
      gpr_free(dump);
    }
  }

  rdma->read_cb = NULL;
  if(rdma->incoming_buffer!=&rdma->temp_buffer)
    rdma->incoming_buffer = NULL;
  // grpc_exec_ctx_sched(exec_ctx, cb, error, NULL);
  grpc_core::ExecCtx::Run(DEBUG_LOCATION, cb,error);
}

#define MAX_READ_IOVEC 4
static void* rdma_continue_read(grpc_rdma *rdma, struct ibv_wc *wc) {
  rdma_message* msg=(rdma_message*)wc->wr_id;
  //char *buffer=msg->msg_content;
  //gpr_log(GPR_DEBUG,"Continue Read,Get a slice");
  // GPR_TIMER_BEGIN("rdma_continue_read", 0);
  //gpr_log(GPR_DEBUG,"A Message of %d Bytes Received",wc->byte_len);
  if(msg->msg_info!=MSGINFO_MESSAGE){
    gpr_mu_lock(&rdma->mu_bufcount);
    rdma->peer_buffer_count+=msg->msg_info;
    //gpr_log(GPR_DEBUG,"A sms");//qazwsx
    gpr_mu_unlock(&rdma->mu_bufcount);
    rdma_post_recv(rdma->content->id,
			  msg,
			  msg,
			  INIT_RECV_BUFFER_SIZE,
			  ((rdma_mem_node*)msg)->mr);
        return(NULL);
  }else{
	  //gpr_log(GPR_DEBUG,"A Message %d",(int)msg->msg_len);//qazwsx
	  if(rdma->incoming_buffer==NULL) 
		  rdma->incoming_buffer=&rdma->temp_buffer;
	  gpr_slice_buffer_add_indexed(rdma->incoming_buffer,
                                       rdma_mm_get_slice(msg));
          rdma_mem_node* node=rdma_mm_pop(rdma->content->manager);
          //gpr_log(GPR_DEBUG,"MR_ADDR=%p",((rdma_mem_node*)msg)->mr);
	  if(!node->mr){
	    node->mr=ibv_reg_mr(
				rdma->content->pd,
				&node->context,
				sizeof(rdma_memory_region),
				IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
	    );
            if(!node->mr) gpr_log(GPR_ERROR,"reg_mr failed:%s",strerror(errno));
	  }
          rdma_post_recv(rdma->content->id,
			  &node->context.msg,
			  &node->context.msg,
			  INIT_RECV_BUFFER_SIZE,
			  node->mr);
          return((void*)msg);
  }
}
static void rdma_clean_failed_wr(grpc_rdma *rdma, struct ibv_wc *wc){
	rdma_mem_node* msg=(rdma_mem_node*)wc->wr_id;
	if(msg) rdma_mm_push(msg);
}
//#define MAX_RETRY_COUNT 2
//#define SLEEP_PERIOD 200
static void rdma_handle_read(void *arg /* grpc_rdma */,
                            grpc_error_handle error) {
  grpc_rdma *rdma = (grpc_rdma *)arg;
  GRPC_ERROR_REF(error);
  grpc_error_handle readerr=error;
  struct ibv_cq *cq;
  struct ibv_wc wc;
  int refilled_bufs = 0;
  bool iseagain=false;
  void* ret;
  rdma_memory_region* sms=NULL;
  readfd_notified(rdma);
  if(rdma->dead) readerr=grpc_error_set_str(error, GRPC_ERROR_STR_DESCRIPTION, "EOF");
  if(readerr==GRPC_ERROR_NONE) {
          void *ctx;
	  unsigned events_completed=0;
	  int get_cqe_result=ibv_get_cq_event(rdma->content->recv_comp_channel,&cq,&ctx);
	  if(get_cqe_result!=0){
	    if(errno==EAGAIN){
	      //gpr_log(GPR_DEBUG,"get_cqe failed,EAGAIN");
	      readfd_notify(rdma);
	      iseagain=true;
	    }else{
	      readerr=GRPC_OS_ERROR(errno,"ibv_get_cq_event");
	    }
	  }
	  if(readerr==GRPC_ERROR_NONE&&!iseagain){
		  while(ibv_poll_cq(cq,1,&wc)){
			  ++events_completed;
			  if(wc.status==IBV_WC_SUCCESS){
				  ret=rdma_continue_read(rdma,&wc);
				  if(ret!=NULL){
				    sms=(rdma_memory_region*)ret;
				    ++refilled_bufs;
				  } 
			  }else{
				  gpr_log(GPR_DEBUG,"An operation failed. OPCODE=%d status=%d wrid=%d",wc.opcode,wc.status,(int)wc.wr_id);
				  gpr_slice_buffer_reset_and_unref(rdma->incoming_buffer);
				  rdma_clean_failed_wr(rdma,&wc);
				  if(!readerr) readerr=GRPC_ERROR_CREATE_FROM_STATIC_STRING("Read Failed");
			  }
		  }
		  ibv_ack_cq_events(cq,events_completed);

	 		  if(0!=ibv_req_notify_cq(cq,0)){
			  gpr_log(GPR_ERROR,"Failed to require notifications.");
			  if(readerr) readerr=grpc_error_set_str(error, GRPC_ERROR_STR_DESCRIPTION, "Require notification failed");
                          else readerr=GRPC_ERROR_CREATE_FROM_STATIC_STRING("Require notification failed");
		  }
	  }
  }else{
	  void* ctx;
	  int get_cqe_result=ibv_get_cq_event(rdma->content->recv_comp_channel,&cq,&ctx);
	  unsigned events_completed=0;
	  if(get_cqe_result==0){
	    while(ibv_poll_cq(cq,1,&wc)){
	      rdma_clean_failed_wr(rdma,&wc);
	      ++events_completed;
	    }
	    ibv_ack_cq_events(cq,events_completed);
	  }
  }
  if(readerr!=GRPC_ERROR_NONE){
      call_read_cb( rdma, readerr);
      RDMA_UNREF(rdma,"read");
  }else{
	  if(iseagain) return;
	  if(sms){
		  //gpr_log(GPR_DEBUG,"Send a short message");//qazwsx
		  sms->sms.msg_info=refilled_bufs;
		  rdma_post_send(rdma->content->id,
				  (void*)SENDCONTEXT_SMS,
				  &(sms->sms),
				  sizeof(rdma_smessage),
				  ((rdma_mem_node*)sms)->mr,
				  0);
      		  call_read_cb( rdma, readerr);
                  RDMA_UNREF(rdma,"read");
	  }else{
		  if(rdma->read_cb)
			  readfd_notify(rdma);
	  }
	  if(rdma->msg_pending&&rdma->peer_buffer_count>0){
		  grpc_error_handle writeerr=GRPC_ERROR_NONE;
		  rdma_flush(rdma,&writeerr);
		  if(writeerr!=GRPC_ERROR_NONE){
			  rdma_on_send_complete(rdma,writeerr);
		  }else{
			  writefd_notify(rdma);
			  //grpc_fd_notify_on_read(exec_ctx,rdma->content->sendfdobj,&rdma->write_closure);
		  }
	  }
  }
}

static void rdma_read(grpc_endpoint *ep,
                     gpr_slice_buffer *incoming_buffer, grpc_closure *cb, bool /*urgent*/) {
  //gpr_log(GPR_DEBUG,"RDMA_READ CALLED");
  grpc_rdma *rdma = (grpc_rdma *)ep;
  //GPR_ASSERT(rdma->read_cb == NULL);
  rdma->read_cb = cb;
  gpr_slice_buffer_reset_and_unref(incoming_buffer);
  if(rdma->incoming_buffer==&rdma->temp_buffer){
        gpr_slice_buffer_swap(rdma->incoming_buffer,incoming_buffer);
        rdma->incoming_buffer=NULL;
        call_read_cb(rdma,GRPC_ERROR_NONE);
  }else{
  	rdma->incoming_buffer = incoming_buffer;
  	RDMA_REF(rdma, "read");
	readfd_notify(rdma);
  }
}
static void rdma_on_send_complete(grpc_rdma *rdma,grpc_error_handle error){
	if(rdma->write_cb==NULL) return;
	//gpr_log(GPR_DEBUG,"CALLED WRITE_CB");
	rdma->outgoing_slice_idx=rdma->outgoing_byte_idx=0;
	// grpc_exec_ctx_sched(exec_ctx, rdma->write_cb, error, NULL);
  grpc_core::ExecCtx::Run(DEBUG_LOCATION, rdma->write_cb,error);
	rdma->write_cb=NULL;
}
/* returns true if done, false if pending; if returning true, *error is set */
#define MAX_WRITE_IOVEC 16
#define MIN_NUM(a,b) ((a)<(b)?(a):(b))
static bool rdma_flush(grpc_rdma *rdma, grpc_error_handle *error) {
  gpr_mu_lock(&rdma->mu_bufcount);
  if(rdma->peer_buffer_count<=0) {
        gpr_log(GPR_DEBUG,"WAIT FOR PARTNER's BUFFER");
	rdma->msg_pending=true;
	gpr_mu_unlock(&rdma->mu_bufcount);
	return(false);
  }
  rdma->msg_pending=false;
  size_t sending_length;
  size_t unwind_slice_idx;
  size_t unwind_byte_idx;
  struct ibv_send_wr workreq,*bad_wr;
  struct ibv_sge sge;
  int result;
  sending_length=0;
  unwind_slice_idx=rdma->outgoing_slice_idx;
  unwind_byte_idx=rdma->outgoing_byte_idx;
  rdma_message *msg=(rdma_message*)rdma->content->send_buffer;
  char* outmemory_now=msg->msg_content;
  size_t slicelength=0,thislength=0,restlength=RDMA_MSG_CONTENT_SIZE;
  for(;unwind_slice_idx<rdma->outgoing_buffer->count;++unwind_slice_idx){
	  slicelength=GPR_SLICE_LENGTH(rdma->outgoing_buffer->slices[unwind_slice_idx])-unwind_byte_idx;
	  thislength=MIN_NUM(slicelength,restlength);
	  memcpy(outmemory_now,
			  GPR_SLICE_START_PTR(rdma->outgoing_buffer->slices[unwind_slice_idx])+unwind_byte_idx,
			  thislength);
	  outmemory_now+=thislength;
          sending_length+=thislength;
	  restlength-=thislength;
	  if(thislength<slicelength) break;
	  unwind_byte_idx=0;
  }
  msg->msg_info=MSGINFO_MESSAGE;
  msg->msg_len=sending_length;
  memset(&workreq,0,sizeof(workreq));
  workreq.opcode = IBV_WR_SEND;
  workreq.wr_id = SENDCONTEXT_DATA;
  workreq.sg_list = &sge;
  workreq.num_sge = 1;
  workreq.send_flags = IBV_SEND_SIGNALED;
  sge.addr = (uintptr_t)rdma->content->send_buffer;
  sge.length = (uint32_t)((uintptr_t)outmemory_now-(uintptr_t)msg);
  sge.lkey = rdma->content->send_buffer_mr->lkey;
  result=ibv_post_send(rdma->content->qp,&workreq,&bad_wr);
  --rdma->peer_buffer_count;
  //gpr_log(GPR_DEBUG,"Send a message %d",(int)msg->msg_len);//qazwsx
  gpr_mu_unlock(&rdma->mu_bufcount);
  if(result==0){
    rdma->outgoing_slice_idx=unwind_slice_idx;
    rdma->outgoing_byte_idx=(unwind_slice_idx>=rdma->outgoing_buffer->count?0:thislength);
    *error=GRPC_ERROR_NONE;
    return(true);
  }else{
    *error = GRPC_OS_ERROR(errno, "sendmsg");
    return(true);
  }
}

static void rdma_handle_write(void *arg /* grpc_rdma */,
                             grpc_error_handle error) {
  grpc_rdma *rdma = (grpc_rdma *)arg;
  GRPC_ERROR_REF(error);
  grpc_error_handle writeerr=error;
  //grpc_closure *cb;
  bool sendctx_has_data=0;
  //bool iseagain=false;
  writefd_notified(rdma);
  //gpr_log(GPR_DEBUG,"HANDLE_WRITE_CALLED");
  if(rdma->dead) writeerr=grpc_error_set_str(error, GRPC_ERROR_STR_DESCRIPTION, "Shutdown");// GRPC_ERROR_CREATE("Shutdown");

  if (writeerr == GRPC_ERROR_NONE) {
	  void *ctx;
	  unsigned events_completed=0;
	  struct ibv_cq *cq;
	  struct ibv_wc wc;
	  int get_cqe_result=ibv_get_cq_event(rdma->content->send_comp_channel,&cq,&ctx);
	  //int retry_count=0;
	  if(0!=get_cqe_result){
		  //if(errno!=EAGAIN||retry_count>MAX_RETRY_COUNT){
			  //if (errno == 11) {}//gpr_log(GPR_DEBUG,"Failed to get events from completion_queue.Errno=%d",errno);
			  //else gpr_log(GPR_ERROR,"Failed to get events from completion_queue.Errno=%d",errno);
		  if(errno==EAGAIN){
//			  iseagain=true;
			  //gpr_log(GPR_DEBUG,"HANDLEWRITE:EAGAIN");
			  writefd_notify(rdma);
			  return;
		  }else{	  	
			  writeerr=GRPC_OS_ERROR(errno,"get_cq_event");
		  }
		//break;
	  }
		  //++retry_count;
		  //usleep(SLEEP_PERIOD);
		  //get_cqe_result=ibv_get_cq_event(rdma->content->send_comp_channel,&cq,&ctx);
	  //}
	  if(writeerr==GRPC_ERROR_NONE){
		  while(ibv_poll_cq(cq,1,&wc)){
			  ++events_completed;
			  if(wc.status!=IBV_WC_SUCCESS){
			    gpr_log(GPR_ERROR,"An operation failed. OPCODE=%d status=%d wrid=%d",wc.opcode,wc.status,(int)wc.wr_id);
			    //FIXME(likaixi added)
			    if(!writeerr) writeerr=grpc_error_set_str(error, GRPC_ERROR_STR_DESCRIPTION, "Read Failed");//GRPC_ERROR_CREATE("Read Failed");
			  }//else{
  			    //gpr_log(GPR_DEBUG,"A Message sent");
			  //}
			  if(wc.wr_id==SENDCONTEXT_DATA){
                            sendctx_has_data=1;
			  }/*else{
			    gpr_log(GPR_DEBUG,"SMS done");
			  }*/
		  }
		  ibv_ack_cq_events(cq,events_completed);
		  if(0!=ibv_req_notify_cq(cq,0)){
			  gpr_log(GPR_ERROR,"Failed to require notifications.");
			  if(!writeerr) writeerr= grpc_error_set_str(error, GRPC_ERROR_STR_DESCRIPTION, "Notify Failed");//GRPC_ERROR_CREATE("Notify Failed");
		  }
	  }
  }
  if(writeerr!=GRPC_ERROR_NONE){
    gpr_log(GPR_ERROR,"Handle_Write Failed");
  }
  if(writeerr||rdma->outgoing_slice_idx>=rdma->outgoing_buffer->count){
	  rdma_on_send_complete(rdma,writeerr);
	  RDMA_UNREF(rdma,"write");
  }else{
          if(sendctx_has_data){
		  if(rdma_flush(rdma,&writeerr)){
			  if(writeerr!=GRPC_ERROR_NONE)
				  rdma_on_send_complete(rdma,writeerr);
			  else
				  writefd_notify(rdma);
				  //grpc_fd_notify_on_read(exec_ctx,rdma->content->sendfdobj,&rdma->write_closure);
		  }else{
	                  //gpr_log(GPR_DEBUG,"Lack of buffer,wait for a while");
	  	          readfd_notify(rdma);
		  }
	  }else{
	     writefd_notify(rdma);
	     //grpc_fd_notify_on_read(exec_ctx,rdma->content->sendfdobj,&rdma->write_closure);
          }
  }
}

static void rdma_write(grpc_endpoint *ep,
                      gpr_slice_buffer *buf, grpc_closure *cb,void* /*arg*/) {
  //gpr_log(GPR_DEBUG,"RDMA_WRITE_CALLED");
  grpc_rdma *rdma = (grpc_rdma *)ep;
  grpc_error_handle error = GRPC_ERROR_NONE;

  if (grpc_rdma_trace) {
    size_t i;

    for (i = 0; i < buf->count; i++) {
      char *data =
          grpc_dump_slice(buf->slices[i], GPR_DUMP_HEX | GPR_DUMP_ASCII);
  //    gpr_log(GPR_DEBUG, "WRITE %p (peer=%s): %s", rdma, rdma->peer_string, data);
      gpr_free(data);
    }
  }

  // GPR_TIMER_BEGIN("rdma_write", 0);
  GPR_ASSERT(rdma->write_cb == NULL);

  if (buf->length == 0) {
    // GPR_TIMER_END("rdma_write", 0);
    // grpc_exec_ctx_sched(exec_ctx, cb, GRPC_ERROR_NONE, NULL);
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, cb,GRPC_ERROR_NONE);
    return;
  }
  rdma->outgoing_buffer = buf;
  rdma->outgoing_slice_idx = 0;
  rdma->outgoing_byte_idx = 0;

  if(rdma_flush(rdma, &error)){
	  if(error!=GRPC_ERROR_NONE)
		  // grpc_exec_ctx_sched(exec_ctx, cb, error, NULL);
      grpc_core::ExecCtx::Run(DEBUG_LOCATION, cb,error);
	  else{
		  rdma->write_cb = cb;
		  RDMA_REF(rdma,"write");
                  writefd_notify(rdma);
		  //grpc_fd_notify_on_read(exec_ctx,rdma->content->sendfdobj,&rdma->write_closure);
	  }
  }else{
//	  gpr_log(GPR_DEBUG,"Lackof buffer,wait for a while");
	  rdma->write_cb = cb;
	  RDMA_REF(rdma,"write");
	  readfd_notify(rdma);
  }
  // GPR_TIMER_END("rdma_write", 0);
}

static void rdma_add_to_pollset(grpc_endpoint *ep,
                               grpc_pollset *pollset) {
  grpc_rdma *rdma = (grpc_rdma *)ep;
  grpc_pollset_add_fd(pollset, rdma->content->recvfdobj);
  grpc_pollset_add_fd(pollset, rdma->content->sendfdobj);
}

static void rdma_add_to_pollset_set(grpc_endpoint *ep,
                                   grpc_pollset_set *pollset_set) {
  grpc_rdma *rdma = (grpc_rdma *)ep;
  grpc_pollset_set_add_fd(pollset_set, rdma->content->recvfdobj);
  grpc_pollset_set_add_fd(pollset_set, rdma->content->sendfdobj);
}
static void rdma_delete_from_pollset_set(grpc_endpoint *ep,
                                   grpc_pollset_set *pollset_set) {
  grpc_rdma *rdma = (grpc_rdma *)ep;
  grpc_pollset_set_del_fd(pollset_set, rdma->content->recvfdobj);
  grpc_pollset_set_del_fd(pollset_set, rdma->content->sendfdobj);
}

static absl::string_view rdma_get_local_address(grpc_endpoint* ep) {
  return std::string("");
}

static int rdma_get_fd(grpc_endpoint* ep) {
  grpc_rdma* rdma = reinterpret_cast<grpc_rdma*>(ep);
  return rdma->fd;
}

static absl::string_view rdma_get_peer(grpc_endpoint *ep) {
  grpc_rdma *rdma = (grpc_rdma *)ep;
  return rdma->peer_string;
}

static bool rdma_can_track_err(grpc_endpoint* ep) {
  grpc_rdma* tcp = reinterpret_cast<grpc_rdma*>(ep);
  if (!grpc_event_engine_can_track_errors()) {
    return false;
  }
  return true;
  // struct sockaddr addr;
  // socklen_t len = sizeof(addr);
  // if (getsockname(tcp->fd, &addr, &len) < 0) {
  //   return false;
  // }
  // return addr.sa_family == AF_INET || addr.sa_family == AF_INET6;
}
//static grpc_workqueue *rdma_get_workqueue(grpc_endpoint *ep) {
//  grpc_rdma *rdma = (grpc_rdma *)ep;
//  return grpc_fd_get_workqueue(rdma->content->recvfdobj);
//}
static const grpc_endpoint_vtable vtable = {
	rdma_read,
	rdma_write,
	rdma_add_to_pollset,
	rdma_add_to_pollset_set,
  rdma_delete_from_pollset_set,
	rdma_shutdown,
	rdma_destroy,
	rdma_get_peer,
  rdma_get_local_address,
  rdma_get_fd,
  rdma_can_track_err
};

grpc_endpoint *grpc_rdma_create(connect_context *c_ctx,
                               const char *peer_string) {
  grpc_rdma *rdma = (grpc_rdma *)gpr_malloc(sizeof(grpc_rdma));
  rdma->base.vtable = &vtable;
  rdma->peer_string = peer_string;
  rdma->fd = grpc_fd_wrapped_fd(c_ctx->recvfdobj);
  rdma->read_cb = NULL;
  rdma->write_cb = NULL;
  rdma->release_fd_cb = NULL;
  rdma->release_fd_in=rdma->release_fd_out = NULL;
  rdma->incoming_buffer = NULL;
  //gpr_log(GPR_DEBUG,"CREATE:RDMA_CM_ID=%p",c_ctx->id);
  //rdma->outgoing_memory=NULL;
  //rdma->outgoing_mr=NULL;
  rdma->dead=false;
  rdma->rflag=false;
  rdma->wflag=false;
  rdma->peer_buffer_count=RDMA_POST_RECV_NUM >> 1;
  gpr_mu_init(&rdma->mu_death);
  gpr_mu_init(&rdma->mu_bufcount);
  gpr_mu_init(&rdma->mu_rflag);
  gpr_mu_init(&rdma->mu_wflag);
  gpr_slice_buffer_init(&rdma->temp_buffer);
  rdma->iov_size = 1;
  /* paired with unref in grpc_rdma_destroy */
  gpr_ref_init(&rdma->refcount, 1);
  //RDMA_REF(rdma,"Born");
  rdma->content = c_ctx;
  rdma_ctx_ref(c_ctx);
  rdma->read_closure.cb = rdma_handle_read;
  rdma->read_closure.cb_arg = rdma;
  rdma->write_closure.cb = rdma_handle_write;
  rdma->write_closure.cb_arg = rdma;
  /* Tell network status tracker about new endpoint */
  //grpc_network_status_register_endpoint(&rdma->base);

  return &rdma->base;
}

int grpc_rdma_fd(grpc_endpoint *ep) {
  grpc_rdma *rdma = (grpc_rdma *)ep;
  GPR_ASSERT(ep->vtable == &vtable);
  return grpc_fd_wrapped_fd(rdma->content->recvfdobj);
}

void grpc_rdma_destroy_and_release_fd(grpc_endpoint *ep,
                                     int *recvfd,int *sendfd, grpc_closure *done) {
  grpc_rdma *rdma = (grpc_rdma *)ep;
  GPR_ASSERT(ep->vtable == &vtable);
  rdma->release_fd_in = recvfd;
  rdma->release_fd_out = sendfd;
  rdma->release_fd_cb = done;
  RDMA_UNREF( rdma, "destroy");
}

static void rdma_sentence_death(grpc_rdma *rdma){
  gpr_mu_lock(&rdma->mu_death);
  rdma->dead=true;
  gpr_mu_unlock(&rdma->mu_death);
}
void  grpc_rdma_sentence_death(grpc_endpoint *ep){
  grpc_rdma *rdma=(grpc_rdma *) ep;
  if(rdma->dead) return;
  //gpr_log(GPR_DEBUG,"GRPC_SENTENCE_DEATH");
  rdma_shutdown(ep,GRPC_ERROR_NONE);
  rdma_sentence_death(rdma);
  //rdma_destroy_fd(ctx,rdma);
  //RDMA_UNREF(rdma,"death");
}
// #endif

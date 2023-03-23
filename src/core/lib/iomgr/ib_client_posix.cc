#include <grpc/support/port_platform.h>

#include "src/core/lib/iomgr/port.h"

#ifdef GRPC_POSIX_SOCKET_TCP_CLIENT

#include "src/core/lib/iomgr/tcp_client.h"

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>

#include "src/core/lib/iomgr/ev_posix.h"
#include "src/core/lib/iomgr/iomgr_internal.h"
#include "src/core/lib/address_utils/sockaddr_utils.h"
#include "src/core/lib/iomgr/socket_utils_posix.h"
#include "src/core/lib/iomgr/tcp_posix.h"
#include "src/core/lib/iomgr/ib_client.h"
#include "src/core/lib/iomgr/timer.h"
#include "src/core/lib/iomgr/unix_sockets_posix.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/channel/channel_args.h"

#include "src/core/lib/surface/api_trace.h"

#include <rdma/rdma_cma.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "src/core/lib/iomgr/rdma_utils_posix.h"
#include "src/core/lib/iomgr/rdma_cm.h"


// extern int grpc_tcp_trace;

struct async_connect{
  gpr_mu mu;
  grpc_fd *connectfd;
  // grpc_core::Timestamp deadline;
  struct rdma_event_channel *ec;
  grpc_timer alarm;
  grpc_closure on_alarm;
  int refs;
  grpc_closure write_closure;
  grpc_pollset_set *interested_parties;
  std::string addr_str;
  grpc_endpoint **ep;
  grpc_closure *closure;
  grpc_channel_args* channel_args;
  grpc_fd *sendfdobj;
  grpc_fd *recvfdobj;
  int time_out;
  int connected;
  int cb_called;
} ;

static void tc_on_alarm(void *acp, grpc_error *error) {
  gpr_log(GPR_INFO,"CLIENT FUNC:tc_on_alarm CALLED");
  async_connect *ac = static_cast<async_connect*>(acp);
  gpr_log(GPR_INFO, "CLIENT_CONNECT: %s: on_alarm: error=%s",ac->addr_str.c_str(), grpc_error_std_string(error).c_str());
  int done;
  gpr_mu_lock(&ac->mu);
  if (ac->connected == 0) {
    if (ac->connectfd != NULL) {
      grpc_fd_shutdown(ac->connectfd,GRPC_ERROR_CREATE_FROM_STATIC_STRING("connect() timed out"));
    }
    ac->time_out = 1;
  }
  done = (--ac->refs == 0);
  gpr_log(GPR_INFO,"ac->refs:%d",ac->refs);
  gpr_mu_unlock(&ac->mu);

  if (done) {
    gpr_log(GPR_INFO,"ac->refs:%d",ac->refs);
    // if (ac->addr_str != NULL) {
    //   gpr_free(ac->addr_str);
    //   ac->addr_str = NULL;
    // }
    // if (ac != NULL) {
      gpr_mu_destroy(&ac->mu);
      grpc_channel_args_destroy(ac->channel_args);
      delete ac;
      // gpr_free(ac->addr_str);
      // gpr_free(ac);
      // ac = NULL;
    // } 
  }
}

#define TIMEOUT_IN_MS 500
static int on_addr_resolved(struct rdma_cm_id *id) {
  struct ibv_qp_init_attr qp_attr;

  struct ibv_pd *pd;
  struct ibv_cq *recv_cq, *send_cq;
  struct ibv_comp_channel *recv_comp_channel, *send_comp_channel;

  struct connect_context *context;
  //struct ibv_mr *recv_buffer_mr;
  struct ibv_mr *send_buffer_mr;
  //char *recv_buffer_region;
  char *send_buffer_region;

  struct ibv_recv_wr wr[RDMA_POST_RECV_NUM], *bad_wr = NULL;
  struct ibv_sge sge[RDMA_POST_RECV_NUM];

  uintptr_t uintptr_addr;
  int i;

  //build context 
  if ((pd = ibv_alloc_pd(id->verbs)) == NULL) {
    gpr_log(GPR_ERROR, "Client: ibv_alloc_pd() failed: %s",strerror(errno));
    return -1;
  } 

  if ((send_comp_channel = ibv_create_comp_channel(id->verbs)) == NULL) { //FIXME
    gpr_log(GPR_ERROR, "Client: ibv_create_comp_channel() failed: %s",strerror(errno));
    return -1;
  }
  if ((recv_comp_channel = ibv_create_comp_channel(id->verbs)) == NULL) { //FIXME
    gpr_log(GPR_ERROR, "Client: ibv_create_comp_channel() failed: %s",strerror(errno));
    return -1;
  }

  if ((send_cq = ibv_create_cq(id->verbs, RDMA_CQ_SIZE, NULL, send_comp_channel, 0)) ==NULL) { //FIXME 
    gpr_log(GPR_ERROR, "Client: ibv_create_cq() failed: %s",strerror(errno));
    return -1;
  }
  if ((recv_cq = ibv_create_cq(id->verbs, RDMA_CQ_SIZE, NULL, recv_comp_channel, 0)) ==NULL) { //FIXME 
    gpr_log(GPR_ERROR, "Client: ibv_create_cq() failed: %s",strerror(errno));
    return -1;
  }

  if ((ibv_req_notify_cq(send_cq, 0)) != 0) {
    gpr_log(GPR_ERROR, "Client: ibv_req_notify_cq() failed: %s",strerror(errno));
    return -1;
  }
  if ((ibv_req_notify_cq(recv_cq, 0)) != 0) {
    gpr_log(GPR_ERROR, "Client: ibv_req_notify_cq() failed: %s",strerror(errno));
    return -1;
  }

  int flags;
  flags = fcntl(send_comp_channel->fd, F_GETFL);
  fcntl(send_comp_channel->fd, F_SETFL, flags | O_NONBLOCK); //set non-blocking
  flags = fcntl(recv_comp_channel->fd, F_GETFL);
  fcntl(recv_comp_channel->fd, F_SETFL, flags | O_NONBLOCK); //set non-blocking


  //build_qp_attr(&qp_attr);
  memset(&qp_attr, 0, sizeof(qp_attr)); 
  qp_attr.send_cq = send_cq;
  qp_attr.recv_cq = recv_cq;
  qp_attr.qp_type = IBV_QPT_RC;
  qp_attr.cap.max_send_wr = RDMA_CQ_SIZE; //TODO
  qp_attr.cap.max_recv_wr = RDMA_CQ_SIZE;
  qp_attr.cap.max_send_sge = 1;
  qp_attr.cap.max_recv_sge = 1;

  if ((rdma_create_qp(id, pd, &qp_attr)) != 0) {
    gpr_log(GPR_ERROR, "Client: rdma_create_qp() failed: %s",strerror(errno));
    return -1;
  }
  gpr_log(GPR_INFO,"The actual size is(%d,%d)",qp_attr.cap.max_send_wr,qp_attr.cap.max_recv_wr);
  send_buffer_region = (char*)gpr_malloc(INIT_RECV_BUFFER_SIZE);
  rdma_mem_manager* manager=rdma_mm_create();
  rdma_mem_node* node;
  for(i=0;i<RDMA_POST_RECV_NUM;++i){
    node=rdma_mm_alloc_node(manager);
    if((node->mr = ibv_reg_mr(
		             pd,
			     &node->context,
                             sizeof(rdma_memory_region),
                             IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE))
        ==NULL) {
	  gpr_log(GPR_ERROR, "Client(%d): ibv_reg_mr() failed: %s",i,strerror(errno));
	  return(-1);
	}
	rdma_mm_push(node);
  }

  if((send_buffer_mr = ibv_reg_mr(
          pd,
          send_buffer_region,
          INIT_RECV_BUFFER_SIZE,
          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE))
      == NULL) {
    gpr_log(GPR_ERROR, "Client: ibv_reg_mr() failed: %s",strerror(errno));
    return -1;
  }


  //build id->context
  context = (struct connect_context *)gpr_malloc(sizeof(struct connect_context));
  memset(context, 0 ,sizeof(struct connect_context));
  gpr_ref_init(&context->refcount, 1);
  // new (&context->refcount) grpc_core::RefCount(1,"rdma");
  context->sendfd = send_comp_channel->fd;
  context->recvfd = recv_comp_channel->fd;
  context->qp = id->qp;
  //context->recv_buffer_region = recv_buffer_region;
  context->send_buffer = send_buffer_region;
  //context->recv_buffer_mr = recv_buffer_mr;
  context->manager=manager;
  context->send_buffer_mr = send_buffer_mr;
  context->id = id;

  context->sendfdobj = NULL;
  context->recvfdobj = NULL;
  context->pd = pd;
  context->send_comp_channel = send_comp_channel;
  context->send_cq = send_cq;
  context->recv_comp_channel = recv_comp_channel;
  context->recv_cq = send_cq;
  id->context = context;


  //post_receives(context);
  //gpr_log(GPR_DEBUG,"POST %d RECEIVES",RDMA_POST_RECV_NUM);
  for (i=0; i<RDMA_POST_RECV_NUM; i++) {
    node=rdma_mm_pop(manager);
    memset(&(wr[i]),0,sizeof(wr[i]));
    uintptr_addr = (uintptr_t)&node->context.msg;
    wr[i].wr_id = uintptr_addr;//用指针来代表wr_id。
    wr[i].next = &(wr[i+1]);
    if ( i == RDMA_POST_RECV_NUM -1) {
      wr[i].next = NULL;
    }
    wr[i].sg_list = &(sge[i]);
    wr[i].num_sge = 1;

    sge[i].addr = uintptr_addr;
    sge[i].length = sizeof(rdma_message);
    sge[i].lkey = node->mr->lkey;

  }

  if ((ibv_post_recv(context->qp, &(wr[0]), &bad_wr)) != 0){
    gpr_log(GPR_ERROR, "Client: ibv_post_recv() failed: %s",strerror(errno));
    return -1;
  }
  if ((rdma_resolve_route(id, TIMEOUT_IN_MS)) != 0){
    gpr_log(GPR_ERROR, "Client: rdma_resolve_route() failed: %s",strerror(errno));
    return -1;
  }
  return 0; 
}

static int on_disconnect(struct rdma_cm_id *id) {
  struct connect_context *context = (struct connect_context *)id->context;
  context->closure=NULL;
  if (context->refcount.count >= 2) {
    grpc_rdma_sentence_death(context->ep);
    // shutdown(context->sendfd,SHUT_RDWR);
    // shutdown(context->recvfd,SHUT_RDWR);
  }
  rdma_ctx_unref(context);
  // std::cout << "src/core/lib/iomgr/ib_client_posix.cc:on_disconnect()" << std::endl;
  return 0;
}

static void client_on_event(void *arg, grpc_error_handle error) {
  async_connect *ac = static_cast<async_connect*>(arg);
  grpc_endpoint **ep = ac->ep;
  struct rdma_event_channel *ec = ac->ec;
  grpc_closure *closure = ac->closure;
  struct connect_context *context = NULL;

  struct rdma_cm_event event_handle;
  struct rdma_cm_event *event = NULL;
  struct rdma_cm_id *id = NULL;

  int timeout;
  int done;
  std::string name;
  // char *name = NULL;
  struct rdma_conn_param cm_params;
  int poll_count = 0;

  GRPC_API_TRACE("client_on_event error begin %s",1, (grpc_error_std_string(error).c_str()));
  // std::cout << "client_on_event error begin " << grpc_error_std_string(error).c_str() << std::endl;
  (void)GRPC_ERROR_REF(error);
  if (error != GRPC_ERROR_NONE ) {
    goto finish;
  }
  
  gpr_mu_lock(&ac->mu);
  timeout = ac->time_out ;
  gpr_mu_unlock(&ac->mu);
  if (timeout == 1) {
    // if (error != GRPC_ERROR_NONE) {
    gpr_log(GPR_ERROR, "Client: Timeout occurred");
    error =
      grpc_error_set_str(error, GRPC_ERROR_STR_OS_ERROR, "Timeout occurred");
    goto finish;
  }
  while (rdma_get_cm_event(ec, &event) != 0) {
    // if (errno == EAGAIN && ++poll_count < 1000){
    if (errno == EAGAIN){
      usleep(50000);
    } else{
      gpr_log(GPR_ERROR, "Client: get_cm_event failed:%d",errno);
      goto finish;
    }
  }

  memcpy(&event_handle, event, sizeof(*event));
  id = event_handle.id;
  rdma_ack_cm_event(event);
  // std::cout << "src/core/lib/iomgr/ib_client_posix.cc:client_on_event() event_handle.event 2" << std::endl;
  switch (event_handle.event) {
    case RDMA_CM_EVENT_ADDR_RESOLVED:
      gpr_log(GPR_INFO, "Client: on_event RDMA_CM_EVENT_ADDR_RESOLVED");
      std::cout << "Client: on_event RDMA_CM_EVENT_ADDR_RESOLVED " << grpc_error_std_string(error).c_str() << std::endl;
      if (on_addr_resolved(id) != 0) {
        error = grpc_error_set_str(error, 
            GRPC_ERROR_STR_DESCRIPTION, "Failed on on_addr_resolved");
        GRPC_API_TRACE("Client: on_event RDMA_CM_EVENT_ADDR_RESOLVED %s",1, (grpc_error_std_string(error).c_str()));
        // std::cout << "Client: on_event RDMA_CM_EVENT_ADDR_RESOLVED " << grpc_error_std_string(error).c_str() << std::endl;
        goto finish;
      }  
      GRPC_API_TRACE("client_on_event case RDMA_CM_EVENT_ADDR_RESOLVED %s",1, (grpc_error_std_string(error).c_str()));
      // std::cout << "client_on_event case RDMA_CM_EVENT_ADDR_RESOLVED " << grpc_error_std_string(error).c_str() << std::endl;
      // std::cout << "Client: on_event RDMA_CM_EVENT_ADDR_RESOLVED" << std::endl;
      break;
    case RDMA_CM_EVENT_ROUTE_RESOLVED:
      gpr_log(GPR_INFO, "Client: on_event RDMA_CM_EVENT_ROUTE_RESOLVED");
      GRPC_API_TRACE("Client: on_event RDMA_CM_EVENT_ROUTE_RESOLVED",0, ());
      std::cout << "Client: on_event RDMA_CM_EVENT_ROUTE_RESOLVED"<< std::endl;
      memset(&cm_params, 0, sizeof(cm_params));
      if ((rdma_connect(id, &cm_params)) != 0) {
        error = grpc_error_set_str(error,
            GRPC_ERROR_STR_DESCRIPTION, "Failed on rdma_connect");
        goto finish;
      }
      break;
    case RDMA_CM_EVENT_ESTABLISHED:
      gpr_log(GPR_INFO, "Client: on_event RDMA_CM_EVENT_ESTABLISHED");
      std::cout << "Client: on_event RDMA_CM_EVENT_ESTABLISHED"<< std::endl;
      gpr_mu_lock(&ac->mu);
      ac->connected = 1;
      gpr_mu_unlock(&ac->mu);

      //grpc_timer_cancel(exec_ctx, &ac->alarm);

      name = "rdma-client:"+ac->addr_str;
      // gpr_asprintf(&name, "rdma-client:%s", ac->addr_str.c_str()); 
      context = (struct connect_context *)id->context;
      context->sendfdobj = grpc_fd_create(context->sendfd, name.c_str(), true);
      context->recvfdobj = grpc_fd_create(context->recvfd, name.c_str(), true);
      context->closure = &ac->write_closure;
      ac->sendfdobj = context->sendfdobj;
      ac->recvfdobj = context->recvfdobj;
      // *ep = grpc_rdma_create(id->context, ac->addr_str);
      *ep = grpc_rdma_create((struct connect_context *)id->context, ac->addr_str.c_str());
      context->ep = *ep;
      std::cout << "src/core/lib/iomgr/ib_client_posix.cc:client_on_event() " << closure->file_created << ":"<< closure->line_created  << std::endl;
      grpc_core::ExecCtx::Run(DEBUG_LOCATION, closure, error);
      ac->cb_called = 1;
      // gpr_free(name);
      break;
    case RDMA_CM_EVENT_DISCONNECTED:
      gpr_log(GPR_INFO, "Client: on_event RDMA_CM_EVENT_DISCONNECTED");
      std::cout << "Client: on_event RDMA_CM_EVENT_DISCONNECTED 1"<< std::endl;
      on_disconnect(id);
      // std::cout << "Client: on_event RDMA_CM_EVENT_DISCONNECTED 2"<< std::endl;
      goto done;
      break;
    default:
      gpr_log(GPR_INFO, "Client: on_event Unknow RDMA_CM_EVENT:%d", event_handle.event);
      GRPC_API_TRACE("Client: on_event Unknow RDMA_CM_EVENT:%d",1, (event_handle.event));
      std::cout << "Client: on_event Unknow RDMA_CM_EVENT:%d " << event_handle.event << std::endl;
      error = grpc_error_set_str(error, GRPC_ERROR_STR_DESCRIPTION, "Get Unknow RDMA_CM_EVENT");
      goto finish;
      break;
  }

  grpc_fd_notify_on_read(ac->connectfd, &ac->write_closure);
  // std::cout << "src/core/lib/iomgr/ib_client_posix.cc:client_on_event() event_handle.event 1" << std::endl;
  GRPC_API_TRACE("client_on_event end switch %s",1, (grpc_error_std_string(error).c_str()));
  // std::cout << "client_on_event end switch " << grpc_error_std_string(error).c_str() << std::endl;
  return;

  finish:
  //grpc_timer_cancel(exec_ctx, &ac->alarm);
  if(error!=GRPC_ERROR_NONE){
    error = grpc_error_set_str(error, GRPC_ERROR_STR_DESCRIPTION, "Failed to connect to remote host");
    error = grpc_error_set_str(error,GRPC_ERROR_STR_TARGET_ADDRESS, ac->addr_str);
    GRPC_API_TRACE("src/core/lib/iomgr/ib_client_posix.cc:client_on_event() in finish %s",1, (grpc_error_std_string(error).c_str()));
  }
  ac->cb_called = 1;

  done:
  if (ac->connectfd != NULL) {
    // grpc_pollset_set_del_fd(ac->interested_parties, ac->connectfd);
    grpc_fd_orphan(ac->connectfd, NULL, NULL, "tcp_client_orphan");
    // std::cout << "src/core/lib/iomgr/ib_client_posix.cc:client_on_event()" << std::endl;
    // grpc_fd_shutdown(ac->connectfd,GRPC_ERROR_CREATE_FROM_STATIC_STRING("connect() close"));
    ac->connectfd = NULL;
  }

  gpr_mu_lock(&ac->mu);
  done = (--ac->refs == 0);
  gpr_log(GPR_INFO,"ac->refs:%d",ac->refs);
  gpr_mu_unlock(&ac->mu);
  if (done) {
    gpr_log(GPR_INFO,"ac->refs:%d",ac->refs);
    // if (ac->addr_str != NULL) {
    //   gpr_free(ac->addr_str);
    //   ac->addr_str = NULL;
    // }
    gpr_mu_destroy(&ac->mu);
    grpc_channel_args_destroy(ac->channel_args);
    delete ac;
    // if (ac != NULL) {
    //   gpr_mu_destroy(&ac->mu);
    //   gpr_free(ac);
    //   ac = NULL;
    // }
  }
  grpc_core::ExecCtx::Run(DEBUG_LOCATION, closure, error);
  GRPC_API_TRACE("client_on_event end all",0, ());
  // std::cout << "client_on_event end all" << std::endl;
  return;
}


// #define TIMEOUT_IN_MS 500
static void rdma_client_connect_impl(grpc_closure *closure, grpc_endpoint **ep,
    grpc_pollset_set *interested_parties,const grpc_channel_args* channel_args,
    const grpc_resolved_address* addr, grpc_core::Timestamp deadline) {
  int connectfd;
  grpc_fd *fdobj;
  *ep = NULL;

  struct rdma_cm_id *conn= NULL;
  struct rdma_event_channel *ec = NULL;

  if ((ec = rdma_create_event_channel()) == NULL) {
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, closure, GRPC_OS_ERROR(errno, "rdma_create_event_channel"));
    return;
  }
  if (rdma_create_id(ec, &conn, NULL, RDMA_PS_TCP) != 0) {
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, closure, GRPC_OS_ERROR(errno, "rdma_create_id"));
    rdma_destroy_event_channel(ec);
    return;
  }

  gpr_log(GPR_INFO, "rdma_client_connect_impl");
  // grpc_rdma_util_print_addr((struct sockaddr *)(addr->addr));

  if (rdma_resolve_addr(conn, NULL, (struct sockaddr *)(addr->addr), gpr_time_to_millis(deadline.as_timespec(GPR_TIMESPAN))) != 0) {
    grpc_core::ExecCtx::Run(DEBUG_LOCATION,closure, GRPC_OS_ERROR(errno, "rdma_resolve_addr"));
    rdma_destroy_id(conn); 
    rdma_destroy_event_channel(ec);
    gpr_log(GPR_ERROR, "Client: rdma_resolve_addr Timeout occurred");
    return;
  }

  int flags;
  flags = fcntl(ec->fd, F_GETFL);
  if (fcntl(ec->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    gpr_log(GPR_ERROR, "Client: fcntl set non-blocking failed");
  }
  connectfd = ec->fd;
  std::string name = absl::StrCat("rdma-client-connection:", grpc_sockaddr_to_uri(addr));
  // std::cout << "src/core/lib/iomgr/ib_client_posix.cc:rdma_client_connect_impl() name: " << name << std::endl;
  fdobj = grpc_fd_create(connectfd, name.c_str(),true); 
  grpc_pollset_set_add_fd(interested_parties, fdobj);

  async_connect* ac = new async_connect();
  ac->closure = closure;
  ac->ec = ec;
  ac->ep = ep;
  ac->connectfd = fdobj;
  ac->interested_parties = interested_parties;
  ac->addr_str = grpc_sockaddr_to_uri(addr);
  GRPC_API_TRACE("src/core/lib/iomgr/ib_client_posix.cc:rdma_client_connect_impl() init mu",0, ());
  // std::cout << "src/core/lib/iomgr/ib_client_posix.cc:rdma_client_connect_impl() init mu" << std::endl;
  gpr_mu_init(&ac->mu);
  ac->refs = 2;
  // ac->write_closure.cb = client_on_event;
  // ac->write_closure.cb_arg = ac;
  GRPC_CLOSURE_INIT(&ac->write_closure, client_on_event, ac,grpc_schedule_on_exec_ctx);
  // GRPC_CLOSURE_INIT(&ac->write_closure, client_on_event, ac,NULL);
  ac->channel_args = grpc_channel_args_copy(channel_args);
  ac->time_out=0;
  ac->connected=0;
  ac->cb_called=0;
  ac->sendfdobj =NULL;
  ac->recvfdobj =NULL;
  gpr_mu_lock(&ac->mu);
  // grpc_timer_init(exec_ctx, &ac->alarm,
  //     gpr_convert_clock_type(deadline, GPR_CLOCK_MONOTONIC),
  //     tc_on_alarm, ac, gpr_now(GPR_CLOCK_MONOTONIC));
  GRPC_CLOSURE_INIT(&ac->on_alarm, tc_on_alarm, ac, grpc_schedule_on_exec_ctx);
  // GRPC_CLOSURE_INIT(&ac->on_alarm, tc_on_alarm, ac, NULL);
  grpc_timer_init(&ac->alarm, deadline, &ac->on_alarm);
  /* lkx810 : Use grpc_fd_notify_on_write() to get rdma ec event 
   * until get event RDMA_CM_EVENT_ESTABLISHED */
  grpc_fd_notify_on_read(ac->connectfd, &ac->write_closure);
  gpr_mu_unlock(&ac->mu);
  GRPC_API_TRACE("src/core/lib/iomgr/ib_client_posix.cc:rdma_client_connect_impl() end",0, ());
  // std::cout << "src/core/lib/iomgr/ib_client_posix.cc:rdma_client_connect_impl() end" << std::endl;
}

grpc_rdma_client_vtable grpc_posix_rdma_client_vtable = {rdma_client_connect_impl};
#endif

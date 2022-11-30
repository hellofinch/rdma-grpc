/* FIXME: "posix" files shouldn't be depending on _GNU_SOURCE */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <grpc/support/port_platform.h>
#include "src/core/lib/iomgr/port.h"

// #ifdef GPR_POSIX_SOCKET

#include "src/core/lib/iomgr/tcp_server.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/sync.h>
#include <grpc/support/time.h>
#include <src/core/lib/gpr/useful.h>

#include "src/core/lib/iomgr/resolve_address.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/address_utils/sockaddr_utils.h"
#include "src/core/lib/iomgr/socket_utils_posix.h"
#include "src/core/lib/iomgr/sockaddr.h"
#include "src/core/lib/iomgr/tcp_posix.h"
#include "src/core/lib/iomgr/unix_sockets_posix.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/resource_quota/api.h"


#include <vector>
#include <rdma/rdma_cma.h>
#include <stdio.h>
#include <string>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "src/core/lib/iomgr/rdma_utils_posix.h"
#include "src/core/lib/iomgr/ib_server.h"
#include "src/core/lib/surface/api_trace.h"

#include "src/core/lib/iomgr/rdma_cm.h"
#include <sys/un.h>

#define MIN_SAFE_ACCEPT_QUEUE_SIZE 100
//static gpr_once s_init_max_accept_queue_size;
//static int s_max_accept_queue_size;

// static gpr_once check_init = GPR_ONCE_INIT;
//static bool has_so_reuseport;

typedef struct grpc_rdma_server grpc_rdma_server;
/* one listening port */
typedef struct grpc_rdma_listener {
  int fd;
  struct rdma_event_channel *ec;
  grpc_fd *emfd;
  struct rdma_cm_id *listener;
  grpc_fd *sendfdobj;
  grpc_fd *recvfdobj;
  grpc_rdma_server *server;
  grpc_resolved_address addr;
  int port;
  unsigned port_index;
  unsigned fd_index;
  grpc_closure read_closure;
  grpc_closure destroyed_closure;
  struct grpc_rdma_listener *next;
  /* When we add a listener, more than one can be created, mainly because of
     IPv6. A sibling will still be in the normal list, but will be flagged
     as such. Any action, such as ref or unref, will affect all of the
     siblings in the list. */
  struct grpc_rdma_listener *sibling;
  int is_sibling;
}grpc_rdma_listener;

/* the overall server */
struct grpc_rdma_server {
  gpr_refcount refs;
  /* Called whenever accept() succeeds on a server port. */
  grpc_rdma_server_cb on_accept_cb=nullptr;
  void *on_accept_cb_arg=nullptr;

  gpr_mu mu;

  /* active port count: how many ports are actually still listening */
  size_t active_ports=0; //
  size_t active_listeners=0;
  /* destroyed port count: how many ports are completely destroyed */
  size_t destroyed_ports=0; //
  size_t destroyed_listeners=0;

  /* is this server shutting down? */
  bool shutdown=false;
   /* have listeners been shutdown? */
  bool shutdown_listeners = false;
  /* use SO_REUSEPORT */
  bool so_reuseport = false;
  /* expand wildcard addresses to a list of all local addresses */
  bool expand_wildcard_addrs = false;
  /* linked list of server ports */
  grpc_rdma_listener *head=nullptr;
  grpc_rdma_listener *tail=nullptr;
  unsigned nports=0; //
  unsigned nlisteners=0; //

  /* List of closures passed to shutdown_starting_add(). */
  grpc_closure_list shutdown_starting{nullptr,nullptr};

  /* shutdown callback */
  grpc_closure *shutdown_complete=nullptr;

  /* all pollsets interested in new connections */
  const std::vector<grpc_pollset*>* pollsets;

  /* next pollset to assign a channel to */
  gpr_atm next_pollset_to_assign =0;
     /* channel args for this server */
  grpc_channel_args* channel_args = nullptr;

// //   /* a handler for external connections, owned */
//   grpc_core::TcpServerFdHandler* fd_handler = nullptr;

  /* used to create slice allocators for endpoints, owned */
  grpc_core::MemoryQuotaRefPtr memory_quota;
};


grpc_error_handle rdma_server_create(grpc_closure *shutdown_complete,const grpc_channel_args* args,
                                   grpc_rdma_server **server) {
  // gpr_once_init(&check_init, init);
  // grpc_rdma_server *s = gpr_malloc(sizeof(grpc_rdma_server));
  GRPC_API_TRACE("src/core/lib/iomgr/ib_server_posix.cc:rdma_server_create() begin",0, ());
  grpc_rdma_server* s = new grpc_rdma_server;
  GRPC_API_TRACE("src/core/lib/iomgr/ib_server_posix.cc:rdma_server_create() new",0, ());
  gpr_ref_init(&s->refs, 1);
  gpr_mu_init(&s->mu);
  s->active_ports = 0;
  s->destroyed_ports = 0;
  s->shutdown = false;
  s->shutdown_starting.head = nullptr;
  s->shutdown_starting.tail = nullptr;
  s->shutdown_complete = shutdown_complete;
  s->on_accept_cb = nullptr;
  s->on_accept_cb_arg = nullptr;
  s->head = nullptr;
  s->tail = nullptr;
  s->nports = 0;
  s->channel_args = grpc_channel_args_copy(args);
  // s->fd_handler = nullptr;
  s->memory_quota =
      grpc_core::ResourceQuotaFromChannelArgs(args)->memory_quota();
  gpr_atm_no_barrier_store(&s->next_pollset_to_assign, 0);
  *server = s;
  GRPC_API_TRACE("src/core/lib/iomgr/ib_server_posix.cc:rdma_server_create() end",0, ());
  return GRPC_ERROR_NONE;
}

static void finish_shutdown(grpc_rdma_server *s) {
    gpr_mu_lock(&s->mu);
  GPR_ASSERT(s->shutdown);
  gpr_mu_unlock(&s->mu);
  if (s->shutdown_complete != NULL) {
    // grpc_exec_ctx_sched(exec_ctx, s->shutdown_complete, GRPC_ERROR_NONE, NULL);
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, s->shutdown_complete,GRPC_ERROR_NONE);
  }

  gpr_mu_destroy(&s->mu);

  while (s->head) {
    grpc_rdma_listener *sp = s->head;
    s->head = sp->next;
    gpr_free(sp);
  }

  grpc_channel_args_destroy(s->channel_args);
  // delete s->fd_handler;
  delete s;
}

static void destroyed_listener(void *server,
                           grpc_error_handle /*error*/) {
  grpc_rdma_server* s = static_cast<grpc_rdma_server*>(server);
  gpr_mu_lock(&s->mu);
  s->destroyed_ports++;
  if (s->destroyed_ports == s->nports) {
    gpr_mu_unlock(&s->mu);
    finish_shutdown(s);
  } else {
    GPR_ASSERT(s->destroyed_ports < s->nports);
    gpr_mu_unlock(&s->mu);
  }
}

/* called when all listening endpoints have been shutdown, so no further
   events will be received on them - at this point it's safe to destroy
   things */
static void deactivated_all_ports(grpc_rdma_server *s) {
  /* delete ALL the things */
  gpr_mu_lock(&s->mu);

  GPR_ASSERT(s->shutdown);

  if (s->head) {
    grpc_rdma_listener *sp;
    for (sp = s->head; sp; sp = sp->next) {
      grpc_unlink_if_unix_domain_socket(&sp->addr);
      // sp->destroyed_closure.cb = destroyed_listener;
      // sp->destroyed_closure.cb_arg = s;
      GRPC_CLOSURE_INIT(&sp->destroyed_closure, destroyed_listener, s,
                        grpc_schedule_on_exec_ctx);
      grpc_fd_orphan(sp->emfd, &sp->destroyed_closure, nullptr,
                     "rdma_listener_shutdown");
    }
    gpr_mu_unlock(&s->mu);
  } else {
    gpr_mu_unlock(&s->mu);
    finish_shutdown(s);
  }
}

static void rdma_server_destroy( grpc_rdma_server *s) {
  gpr_mu_lock(&s->mu);

  GPR_ASSERT(!s->shutdown);
  s->shutdown = true;

  /* shutdown all fd's */
  if (s->active_ports) {
    grpc_rdma_listener *sp;
    for (sp = s->head; sp; sp = sp->next) {
      grpc_fd_shutdown(sp->emfd, GRPC_ERROR_CREATE_FROM_STATIC_STRING("Server destroyed"));
    }
    gpr_mu_unlock(&s->mu);
  } else {
    gpr_mu_unlock(&s->mu);
    deactivated_all_ports(s);
  }
}

static int on_connect_request(struct rdma_cm_id *id) {
    struct ibv_qp_init_attr qp_attr;
    struct rdma_conn_param cm_params;

    struct ibv_pd *pd = NULL;
    struct ibv_cq *send_cq = NULL;
    struct ibv_cq *recv_cq = NULL;
    struct ibv_comp_channel *send_comp_channel = NULL;
    struct ibv_comp_channel *recv_comp_channel = NULL;

    struct connect_context *context = NULL;
    //int pollfd;
    //struct ibv_qp *qp;
    //struct ibv_mr *recv_buffer_mr = NULL;
    struct ibv_mr *send_buffer_mr = NULL;
    //char *recv_buffer_region = NULL;
    char *send_buffer_region = NULL;

    struct ibv_recv_wr wr[RDMA_POST_RECV_NUM], *bad_wr = NULL;
    struct ibv_sge sge[RDMA_POST_RECV_NUM];
    int i;
    uintptr_t uintptr_addr;

    //build_context(id->verbs);
    if ((pd = ibv_alloc_pd(id->verbs)) == NULL) {
         gpr_log(GPR_ERROR, "SERVER: ibv_alloc_pd() failed: %s",strerror(errno));
         return -1;
    }
    if ((send_comp_channel = ibv_create_comp_channel(id->verbs)) == NULL) { //FIXME
         gpr_log(GPR_ERROR, "SERVE: ibv_create_comp_channel() failed: %s",strerror(errno));
         return -1;
    }
    if ((recv_comp_channel = ibv_create_comp_channel(id->verbs)) == NULL) { //FIXME
         gpr_log(GPR_ERROR, "SERVER: ibv_create_comp_channel() failed: %s",strerror(errno));
         return -1;
    }
    if ((send_cq = ibv_create_cq(id->verbs, RDMA_CQ_SIZE, NULL, send_comp_channel, 0)) ==NULL) { //FIXME
         gpr_log(GPR_ERROR, "SERVER: ibv_create_cq() failed: %s",strerror(errno));
         return -1;
    }
    if ((recv_cq = ibv_create_cq(id->verbs, RDMA_CQ_SIZE, NULL, recv_comp_channel, 0)) ==NULL) { //FIXME
         gpr_log(GPR_ERROR, "SERVER: ibv_create_cq() failed: %s",strerror(errno));
         return -1;
    }
    if ((ibv_req_notify_cq(recv_cq, 0)) != 0) {
         gpr_log(GPR_ERROR, "SERVER: ibv_req_notify_cq() failed: %s",strerror(errno));
         return -1;
    }
    if ((ibv_req_notify_cq(send_cq, 0)) != 0) {
         gpr_log(GPR_ERROR, "SERVER: ibv_req_notify_cq() failed: %s",strerror(errno));
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
    qp_attr.cap.max_send_wr =  RDMA_CQ_SIZE; //TODO
    qp_attr.cap.max_recv_wr =  RDMA_CQ_SIZE;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;
    //id->qp=NULL;
    if ((rdma_create_qp(id, pd, &qp_attr)) != 0) {
         gpr_log(GPR_ERROR, "SERVER: rdma_create_qp() failed: %s",strerror(errno));
         return -1;
    }
 
   //register_memory(context);
   //recv_buffer_region = gpr_malloc(INIT_RECV_BUFFER_SIZE * RDMA_POST_RECV_NUM); 
    send_buffer_region = (char*)gpr_malloc(INIT_RECV_BUFFER_SIZE); 
    
    /*if((recv_buffer_mr = ibv_reg_mr(
                pd,
                recv_buffer_region,
                INIT_RECV_BUFFER_SIZE * RDMA_POST_RECV_NUM,
                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE))
      == NULL) {
         gpr_log(GPR_ERROR, "CLIENT: ibv_reg_mr() failed: %s",strerror(errno));
         return -1;
    }*/
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
	  gpr_log(GPR_ERROR, "SERVER: ibv_reg_mr() failed: %s",strerror(errno));
	  return(-1);
	}
      rdma_mm_push(node);
    }

    if((send_buffer_mr = ibv_reg_mr(
                pd,
                send_buffer_region,
                INIT_RECV_BUFFER_SIZE ,
                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE))
      == NULL) {
         gpr_log(GPR_ERROR, "SERVER: ibv_reg_mr() failed: %s",strerror(errno));
         return -1;
    }

    //build id->context
    context = (struct connect_context *)gpr_malloc(sizeof(struct connect_context));
    memset(context, 0, sizeof(struct connect_context));
    gpr_ref_init(&context->refcount, 1);
    context->sendfd = send_comp_channel->fd;
    context->recvfd = recv_comp_channel->fd;
    context->qp = id->qp;
    //context->recv_buffer_region = recv_buffer_region;
    //context->recv_buffer_mr = recv_buffer_mr;
    context->manager=manager;
    context->send_buffer = send_buffer_region;
    context->send_buffer_mr = send_buffer_mr;
    context->id = id;
    
    context->pd = pd;
    context->recv_comp_channel = recv_comp_channel;
    context->send_comp_channel = send_comp_channel;
    context->recv_cq = recv_cq;
    context->send_cq = send_cq;
    
    context->sendfdobj = NULL;
    context->recvfdobj = NULL;
    id->context = context;

    //post_receives(context);
    for (i=0; i<RDMA_POST_RECV_NUM; i++) {
	node=rdma_mm_pop(manager);
        memset(&(wr[i]),0,sizeof(wr[i]));
        uintptr_addr = (uintptr_t)&node->context.msg;
        wr[i].wr_id = uintptr_addr;
        wr[i].next = &(wr[i+1]);
        if ( i == RDMA_POST_RECV_NUM - 1) {
             wr[i].next = NULL;
        }
        wr[i].sg_list = &(sge[i]);
        wr[i].num_sge = 1;

        sge[i].addr = uintptr_addr;
        sge[i].length = sizeof(rdma_message);
        sge[i].lkey = node->mr->lkey;

    }
    //gpr_log(GPR_DEBUG,"%d memory nodes left in the manager",manager->node_count);
    if ((ibv_post_recv(context->qp, &(wr[0]), &bad_wr)) != 0){
            gpr_log(GPR_ERROR, "SERVER: ibv_post_recv() failed: %s",strerror(errno));
            return -1;
    }

    //memset(&wr,0,sizeof(wr));
    //uintptr_addr = (uintptr_t)context->recv_buffer_region;
    //wr.wr_id = uintptr_addr;
    //wr.next = NULL;
    //wr.sg_list = &sge;
    //wr.num_sge = 1;

    //sge.addr = uintptr_addr;
    //sge.length = INIT_RECV_BUFFER_SIZE;
    //sge.lkey = recv_buffer_mr->lkey;

    //if ((ibv_post_recv(context->qp, &wr, &bad_wr)) != 0){
    //        gpr_log(GPR_ERROR, "CLIENT: ibv_post_recv() failed: %s",strerror(errno));
    //        return -1;
    //}

    memset(&cm_params, 0, sizeof(cm_params));
    if ((rdma_accept(id, &cm_params)) != 0) {
	gpr_log(GPR_ERROR, "CLIENT: rdma_accept() failed: %s",strerror(errno));
	return -1;
    }


    return 0;
}

static int on_disconnect(struct rdma_cm_id *id) {
    struct connect_context *context = (struct connect_context *)id->context;
    if (context->refcount.count >= 2) {
        grpc_rdma_sentence_death(context->ep);
    }
    rdma_ctx_unref(context);

    
    return 0;
}

char *my_grpc_sockaddr_to_uri_unix_if_possible(const struct sockaddr *addr) {
  if (addr->sa_family != AF_UNIX) {
    return NULL;
  }
  const auto* unix_addr = reinterpret_cast<const struct sockaddr_un*>(addr);
  return (char*)absl::StrCat("unix:", unix_addr->sun_path).c_str();
}

/* event manager callback when reads are ready */
//static void on_read(grpc_exec_ctx *exec_ctx, void *arg, grpc_error *err) {
static int on_connection( grpc_rdma_listener *sp,void *ctx) {
  struct connect_context *context = (struct connect_context *)ctx;

  grpc_rdma_server_acceptor acceptor = {sp->server, sp->port_index,sp->fd_index}; 
  grpc_pollset *read_notifier_pollset = NULL;
  struct sockaddr *addr;
  char *addr_str;
  char *name;

  read_notifier_pollset =
      (*(sp->server->pollsets))[static_cast<size_t>(gpr_atm_no_barrier_fetch_add(
                               &sp->server->next_pollset_to_assign, 1)) %
                           sp->server->pollsets->size()];
   
    addr = rdma_get_peer_addr(context->id);
    // addr_str = grpc_sockaddr_to_uri((struct sockaddr *)addr);
    addr_str = my_grpc_sockaddr_to_uri_unix_if_possible(addr);
    gpr_asprintf(&name, "tcp-server-connection:%s", addr_str);
    /*
    if (grpc_tcp_trace) {
      gpr_log(GPR_DEBUG, "SERVER_CONNECT: incoming connection: %s", addr_str);
    }
*/


    if (read_notifier_pollset == NULL) {
      gpr_log(GPR_ERROR, "Read notifier pollset is not set on the fd");
      //goto error;
      return -1;
    }

    context->sendfdobj = grpc_fd_create(context->sendfd, name,true);
    context->recvfdobj = grpc_fd_create(context->recvfd, name,true);
    grpc_pollset_add_fd(read_notifier_pollset, context->sendfdobj);
    grpc_pollset_add_fd(read_notifier_pollset, context->recvfdobj);

    context->ep = grpc_rdma_create(context, addr_str);

    //grpc_rdma_create(fdobj, GRPC_TCP_DEFAULT_READ_SLICE_SIZE, addr_str), 
    sp->server->on_accept_cb(
        sp->server->on_accept_cb_arg,
	context->ep,
        read_notifier_pollset, &acceptor);

    gpr_free(name);
    gpr_free(addr_str);
  //}

  //GPR_UNREACHABLE_CODE(return );

//error:
 return 0;
 }


static void grpc_rdma_server_on_event(void *arg,grpc_error_handle err) {
    grpc_rdma_listener *sp = static_cast<grpc_rdma_listener*>(arg);
    struct rdma_event_channel *ec = sp->ec;
    struct rdma_cm_event *event = nullptr;
    struct rdma_cm_id *id = nullptr;
    //static struct rdma_cm_event event_handle;
    int ret = 0;

    if (err != GRPC_ERROR_NONE ) {
	goto error;
    } 

    grpc_fd_notify_on_read(sp->emfd, &sp->read_closure); //poll again

    if (rdma_get_cm_event(ec, &event) != 0 ) {
	    return;
    }

    id = event->id;
    switch (event->event) {
        case RDMA_CM_EVENT_CONNECT_REQUEST:
	    gpr_log(GPR_DEBUG, "Server: on_event RDMA_CM_EVENT_CONNECT_REQUEST");
            ret = on_connect_request(event->id);
            rdma_ack_cm_event(event);
            break;
        case RDMA_CM_EVENT_ESTABLISHED:
	    gpr_log(GPR_DEBUG, "Server: on_event RDMA_CM_EVENT_ESTABLISHED");
            ret = on_connection(sp, event->id->context);
            rdma_ack_cm_event(event);
            break;
        case RDMA_CM_EVENT_DISCONNECTED:
	    gpr_log(GPR_DEBUG, "Server: on_event RDMA_CM_EVENT_DISCONNECTED");
            rdma_ack_cm_event(event);
            ret = on_disconnect(id);
            break;
        case RDMA_CM_EVENT_TIMEWAIT_EXIT:
	    gpr_log(GPR_DEBUG,"Server: TIMEWAIT_EXIT");
	    ret=0;
            break;
        default:
	    gpr_log(GPR_ERROR, "Server: on_event Unknow RDMA_CM_EVENT:%d", event->event);
            ret = -1;
            break;
    }

    if (ret != 0) {
        goto error;
    }
    return;
error:
    gpr_mu_lock(&sp->server->mu);
    if (0 == --sp->server->active_ports) {
        gpr_mu_unlock(&sp->server->mu);
        deactivated_all_ports(sp->server);
    } else {
        gpr_mu_unlock(&sp->server->mu);
    }
}

static grpc_error_handle add_listener_to_server(grpc_rdma_server *s,
                                         const grpc_resolved_address *addr,unsigned port_index,
                                         unsigned fd_index,
                                         grpc_rdma_listener **server_listener) {
  grpc_rdma_listener *sp = NULL;
  int port = -1;
  // char *addr_str;
  // char *name;
  int connectfd;
  int flags;

  grpc_error_handle err = GRPC_ERROR_NONE; 

  //grpc_error *err = prepare_listener(listener, addr, addr_len, &port);
  struct rdma_cm_id *listener = NULL;
  struct rdma_event_channel *ec = NULL;
  // gpr_log(GPR_DEBUG, "add_listener_to_server");
  // grpc_rdma_util_print_addr(reinterpret_cast<const grpc_sockaddr*>(addr->addr));
  if ((ec = rdma_create_event_channel()) == NULL) {
    //grpc_exec_ctx_sched(exec_ctx, closure, GRPC_OS_ERROR(errno, "rdma_create_event_channel"), NULL);
	  return GRPC_OS_ERROR(errno, "rdma_create_event_channel");
  }
  if (rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP) != 0) {
    //grpc_exec_ctx_sched(exec_ctx, closure, GRPC_OS_ERROR(errno, "rdma_create_id"), NULL);
	  rdma_destroy_event_channel(ec);
	  return GRPC_OS_ERROR(errno, "rdma_create_id");
  }

  if (rdma_bind_addr(listener,(grpc_sockaddr*)(addr->addr)) != 0) {
    //grpc_exec_ctx_sched(exec_ctx, closure, GRPC_OS_ERROR(errno, "rdma_bind_addr"), NULL);
	  rdma_destroy_id(listener);
	  rdma_destroy_event_channel(ec);
	  return GRPC_OS_ERROR(errno, "rdma_bind_addr");
  }

  if((rdma_listen(listener, 128)) != 0) { /* TODO get_max_accept_queue_size() == 128? */ 
          //grpc_exec_ctx_sched(exec_ctx, closure, GRPC_OS_ERROR(errno, "rdma_listen"), NULL);
          rdma_destroy_id(listener);
          rdma_destroy_event_channel(ec);
          return GRPC_OS_ERROR(errno, "rdma_listen");
  }
  port = ntohs(rdma_get_src_port(listener));

  flags = fcntl(ec->fd, F_GETFL);
  fcntl(ec->fd, F_SETFL, flags | O_NONBLOCK); //set non-blocking
  connectfd = ec->fd;

  if (err == GRPC_ERROR_NONE) {
    GPR_ASSERT(port > 0);
    // std::string addr_string=grpc_sockaddr_to_string(addr, true);
    // std::cout << "addr_string " << addr_string << std::endl;
    std::string name = "tcp-server-listener:"+grpc_sockaddr_to_string(addr, true);
    // gpr_asprintf(&name, "tcp-server-listener:%s", addr_string.c_str());
    gpr_mu_lock(&s->mu);
    s->nports++;
    GPR_ASSERT(!s->on_accept_cb && "must add ports before starting server");
    sp = static_cast<grpc_rdma_listener*>(gpr_malloc(sizeof(grpc_rdma_listener)));
    sp->next = NULL;
    if (s->head == NULL) {
      s->head = sp;
    } else {
      s->tail->next = sp;
    }
    s->tail = sp;
    sp->server = s;
    //sp->fd = connectfd;
    sp->ec = ec;
    sp->listener = listener;
    sp->emfd = grpc_fd_create(connectfd, name.c_str(),true);
    // memcpy(sp->addr, addr, addr.len);
    sp->addr = *addr;
    // sp->addr_len = addr_len;
    sp->port = port;
    sp->port_index = port_index;
    sp->fd_index = fd_index;
    sp->is_sibling = 0;
    sp->sibling = NULL;
    GPR_ASSERT(sp->emfd);
    gpr_mu_unlock(&s->mu);
    // gpr_free(addr_str);
    // gpr_free(name);
  }
  *server_listener = sp;
  return err;
}

grpc_error_handle rdma_server_add_port(grpc_rdma_server *s, const grpc_resolved_address* addr, int *out_port) {
  grpc_rdma_listener *sp;
  grpc_resolved_address sockname_temp;
  int requested_port = grpc_sockaddr_get_port(addr);
  unsigned port_index = 0;
  unsigned fd_index = 0;
  grpc_error_handle err;
  *out_port = -1;
  if (s->tail != nullptr) {
    port_index = s->tail->port_index + 1;
  }
  grpc_unlink_if_unix_domain_socket(addr);
  /* Check if this is a wildcard port, and if so, try to keep the port the same
     as some previously created listener. */
  if (requested_port == 0) {
    for (sp = s->head; sp; sp = sp->next) {
        sockname_temp.len =static_cast<socklen_t>(sizeof(struct sockaddr_storage));
        if (0 ==
          getsockname(sp->fd,
                      reinterpret_cast<grpc_sockaddr*>(&sockname_temp.addr),
                      &sockname_temp.len)) {
          int used_port = grpc_sockaddr_get_port(&sockname_temp);
          if (used_port > 0) {
            memcpy(&sockname_temp, addr, sizeof(grpc_resolved_address));
            grpc_sockaddr_set_port(&sockname_temp, used_port);
            requested_port = used_port;
            addr = &sockname_temp;
            break;
          }
        }
    }
  }
  if ((err= add_listener_to_server(s, addr,port_index, fd_index, &sp)) == GRPC_ERROR_NONE) {
      *out_port = sp->port;
      }
    return err;
}

/* Return listener at port_index or NULL. Should only be called with s->mu
   locked. */
static grpc_rdma_listener* get_port_index(grpc_rdma_server* s, unsigned port_index) {
  unsigned num_ports = 0;
  grpc_rdma_listener* sp;
  for (sp = s->head; sp; sp = sp->next) {
    if (!sp->is_sibling) {
      if (++num_ports > port_index) {
        return sp;
      }
    }
  }
  return nullptr;
}

unsigned rdma_server_port_fd_count(grpc_rdma_server *s,unsigned port_index) {
  unsigned num_fds = 0;
  gpr_mu_lock(&s->mu);
  grpc_rdma_listener *sp=get_port_index(s,port_index);
  for (; sp; sp = sp->sibling) {
    ++num_fds;
  }
  gpr_mu_unlock(&s->mu);
  return num_fds;
}

int rdma_server_port_fd(grpc_rdma_server *s, unsigned port_index,
                            unsigned fd_index) {
  gpr_mu_lock(&s->mu);
  grpc_rdma_listener *sp=get_port_index(s,port_index);
  for (; sp; sp = sp->sibling, --fd_index){
if (fd_index==0) {
      gpr_mu_unlock(&s->mu);
      return sp->fd;
  }
  };
  gpr_mu_unlock(&s->mu);
  return -1;
}

struct sockaddr *grpc_rdma_server_port_addr(grpc_rdma_server *s, unsigned port_index,unsigned fd_index) {
  grpc_rdma_listener *sp;
  for (sp = s->head; sp && port_index != 0; sp = sp->next) {
    if (!sp->is_sibling) {
      --port_index;
    }
  }
  for (; sp && fd_index != 0; sp = sp->sibling, --fd_index)
    ;
  if (sp) {
    return rdma_get_local_addr(sp->listener);
  } else {
    return NULL;
  }
}

void rdma_server_start(grpc_rdma_server *s,
                           const std::vector<grpc_pollset*>* pollsets,
                           grpc_rdma_server_cb on_accept_cb,
                           void *on_accept_cb_arg) {
  size_t i;
  grpc_rdma_listener *sp;
  GPR_ASSERT(on_accept_cb);
  gpr_mu_lock(&s->mu);
  GPR_ASSERT(!s->on_accept_cb);
  GPR_ASSERT(s->active_ports == 0);
  s->on_accept_cb = on_accept_cb;
  s->on_accept_cb_arg = on_accept_cb_arg;
  s->pollsets = pollsets;
  sp = s->head;
  while (sp != nullptr) {
     for (i = 0; i < pollsets->size(); i++) {
        grpc_pollset_add_fd((*pollsets)[i], sp->emfd);
      }
      GRPC_CLOSURE_INIT(&sp->read_closure, grpc_rdma_server_on_event, sp,grpc_schedule_on_exec_ctx);
      grpc_fd_notify_on_read(sp->emfd, &sp->read_closure);
      s->active_ports++;
      sp = sp->next;
  }
  gpr_mu_unlock(&s->mu);
}

grpc_rdma_server *rdma_server_ref(grpc_rdma_server *s) {
  gpr_ref_non_zero(&s->refs);
  return s;
}

void rdma_server_shutdown_starting_add(grpc_rdma_server *s,
                                           grpc_closure *shutdown_starting) {
  gpr_mu_lock(&s->mu);
  grpc_closure_list_append(&s->shutdown_starting, shutdown_starting,
                           GRPC_ERROR_NONE);
  gpr_mu_unlock(&s->mu);
}

void rdma_server_unref(grpc_rdma_server *s) {
  if (gpr_unref(&s->refs)) {
    /* Complete shutdown_starting work before destroying. */
    // grpc_exec_ctx local_exec_ctx = GRPC_EXEC_CTX_INIT;
    gpr_mu_lock(&s->mu);
    grpc_core::ExecCtx::RunList(DEBUG_LOCATION, &s->shutdown_starting);
    // grpc_exec_ctx_enqueue_list(&local_exec_ctx, &s->shutdown_starting, NULL);
    gpr_mu_unlock(&s->mu);
    // if (exec_ctx == NULL) {
      // grpc_exec_ctx_flush(&local_exec_ctx);
      // rdma_server_destroy(&local_exec_ctx, s);
      // grpc_exec_ctx_finish(&local_exec_ctx);
    // } else {
      // grpc_exec_ctx_finish(&local_exec_ctx);
      rdma_server_destroy(s);
    // }
  }
}

void rdma_server_shutdown_listeners(grpc_rdma_server *s) {
  gpr_mu_lock(&s->mu);
  /* shutdown all fd's */
  if (s->active_ports) {
    grpc_rdma_listener *sp;
    for (sp = s->head; sp; sp = sp->next) {
      grpc_fd_shutdown(sp->emfd,GRPC_ERROR_CREATE_FROM_STATIC_STRING("Server shutdown"));
    }
  }
  gpr_mu_unlock(&s->mu);
}

// class ExternalConnectionHandler : public grpc_core::TcpServerFdHandler {
//  public:
//   explicit ExternalConnectionHandler(grpc_tcp_server* s) : s_(s) {}

//   // TODO(yangg) resolve duplicate code with on_read
//   void Handle(int listener_fd, int fd, grpc_byte_buffer* buf) override {
//     grpc_pollset* read_notifier_pollset;
//     grpc_resolved_address addr;
//     memset(&addr, 0, sizeof(addr));
//     addr.len = static_cast<socklen_t>(sizeof(struct sockaddr_storage));
//     grpc_core::ExecCtx exec_ctx;

//     if (getpeername(fd, reinterpret_cast<struct sockaddr*>(addr.addr),
//                     &(addr.len)) < 0) {
//       gpr_log(GPR_ERROR, "Failed getpeername: %s", strerror(errno));
//       close(fd);
//       return;
//     }
//     (void)grpc_set_socket_no_sigpipe_if_possible(fd);
//     std::string addr_str = grpc_sockaddr_to_uri(&addr);
//     if (grpc_tcp_trace.enabled()) {
//       gpr_log(GPR_INFO, "SERVER_CONNECT: incoming external connection: %s",
//               addr_str.c_str());
//     }
//     std::string name = absl::StrCat("tcp-server-connection:", addr_str);
//     grpc_fd* fdobj = grpc_fd_create(fd, name.c_str(), true);
//     read_notifier_pollset =
//         (*(s_->pollsets))[static_cast<size_t>(gpr_atm_no_barrier_fetch_add(
//                               &s_->next_pollset_to_assign, 1)) %
//                           s_->pollsets->size()];
//     grpc_pollset_add_fd(read_notifier_pollset, fdobj);
//     grpc_tcp_server_acceptor* acceptor =
//         static_cast<grpc_tcp_server_acceptor*>(gpr_malloc(sizeof(*acceptor)));
//     acceptor->from_server = s_;
//     acceptor->port_index = -1;
//     acceptor->fd_index = -1;
//     acceptor->external_connection = true;
//     acceptor->listener_fd = listener_fd;
//     acceptor->pending_data = buf;
//     s_->on_accept_cb(s_->on_accept_cb_arg,
//                      grpc_tcp_create(fdobj, s_->channel_args, addr_str),
//                      read_notifier_pollset, acceptor);
//   }

//  private:
//   grpc_tcp_server* s_;
// };

// grpc_core::TcpServerFdHandler* grpc_rdma_server_create_fd_handler(
//     grpc_tcp_server* s) {
//   s->fd_handler = new ExternalConnectionHandler(s);
//   return s->fd_handler;
// }

grpc_rdma_server_vtable grpc_posix_rdma_server_vtable = {
    rdma_server_create,        rdma_server_start,
    rdma_server_add_port,      //grpc_rdma_server_create_fd_handler,
    rdma_server_port_fd_count, rdma_server_port_fd,
    rdma_server_ref,           rdma_server_shutdown_starting_add,
    rdma_server_unref,         rdma_server_shutdown_listeners};

// #endif

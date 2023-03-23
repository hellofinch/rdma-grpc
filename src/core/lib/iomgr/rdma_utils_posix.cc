
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <rdma/rdma_cma.h>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/sync.h>
#include <grpc/support/time.h>
#include "src/core/lib/gpr/useful.h"

#include "src/core/lib/iomgr/rdma_utils_posix.h" 
void rdma_ctx_free(connect_context *);
// void grpc_rdma_util_print_addr(const struct sockaddr *addr);

// void grpc_rdma_util_print_addr(const struct sockaddr *addr){
//   GPR_ASSERT( ((struct sockaddr*)addr)->sa_family == AF_INET ||  ((struct sockaddr*)addr)->sa_family == AF_INET6 );
//   char *addr_str;
//   grpc_sockaddr_to_string(&addr_str, (struct sockaddr *)addr, 1);
//   if ( ((struct sockaddr*)addr)->sa_family == AF_INET ) {
// 	gpr_log(GPR_DEBUG,"ADDR: AF_INET addr:%s port:%d",addr_str,grpc_sockaddr_get_port((struct sockaddr *)addr));
//   }
//   else if ( ((struct sockaddr*)addr)->sa_family == AF_INET6 ) {
// 	gpr_log(GPR_DEBUG,"ADDR: AF_INET6 addr:%s port:%d",addr_str,grpc_sockaddr_get_port((struct sockaddr *)addr));
//   }
//   gpr_free(addr_str);
// }



void die(const char *reason) {
    fprintf(stderr, "%s\n", reason);
    fprintf(stderr, "%d", errno);
    exit(1);
}


void rdma_ctx_unref(struct connect_context *context) {
  if (gpr_unref(&context->refcount)) {
  // if (context->refcount.Unref()) {
    rdma_ctx_free(context);
  }
}

void rdma_ctx_ref(struct connect_context *context) { 
  gpr_ref(&context->refcount); 
  // context->refcount.Ref(); 
}
void rdma_ctx_free(struct connect_context *context) {
  grpc_fd_orphan(context->sendfdobj,NULL,NULL,"RDMASENDOBJ_FREE");
  grpc_fd_orphan(context->recvfdobj,NULL,NULL,"RDMARECVOBJ_FREE");
  ibv_dereg_mr(context->send_buffer_mr); 
  ibv_dealloc_pd(context->pd);
  ibv_destroy_cq(context->send_cq);
  ibv_destroy_cq(context->recv_cq);
  gpr_free(context->send_buffer);
  rdma_mm_unref(context->manager);

  rdma_destroy_id(context->id);
  gpr_free(context);
}

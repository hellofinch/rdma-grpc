#ifndef GRPC_CORE_LIB_IOMGR_RDMA_VERBS_UTILS_POSIX_H
#define GRPC_CORE_LIB_IOMGR_RDMA_VERBS_UTILS_POSIX_H

#include <sys/socket.h>
#include <unistd.h>

#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/ev_posix.h"

#include "src/core/lib/iomgr/iomgr_internal.h"
#include "src/core/lib/address_utils/sockaddr_utils.h"
#include "src/core/lib/iomgr/socket_utils_posix.h"
#include "src/core/lib/iomgr/tcp_posix.h"
#include "src/core/lib/iomgr/timer.h"
#include "src/core/lib/iomgr/unix_sockets_posix.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/iomgr/rdma_memory_management.h"

#include <grpc/support/sync.h>
#include "src/core/lib/iomgr/endpoint.h"
void die(const char *reason);
#define TEST_NZ(x) do { if ( (x)) die("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_Z(x)  do { if (!(x)) die("error: " #x " failed (returned zero/null)."); } while (0)

#define RDMA_CQ_SIZE 128
#define RDMA_POST_RECV_NUM 64

typedef struct connect_context connect_context;
struct connect_context {
    grpc_fd *sendfdobj;/*int pollfd;*/
    grpc_fd *recvfdobj;/*int pollfd;*/
    int sendfd;
    int recvfd;

    struct rdma_cm_id *id;
    struct ibv_qp *qp;

    // struct ibv_mr *recv_buffer_mr;
    // struct ibv_mr *send_buffer_mr;
    // char *recv_buffer_region;
    // char *send_buffer_region;
    char *send_buffer;
    struct ibv_mr *send_buffer_mr;
    rdma_mem_manager *manager;

    struct ibv_pd *pd;
    struct ibv_pd *ctl_pd;

    struct ibv_cq *send_cq;
    struct ibv_cq *recv_cq;
    struct ibv_comp_channel *recv_comp_channel;
    struct ibv_comp_channel *send_comp_channel;

    grpc_endpoint *ep;
    grpc_closure *closure;
    gpr_refcount refcount;
    //buffer_size;
};

void rdma_ctx_ref(connect_context *ctx);
void rdma_ctx_unref(connect_context *ctx);
// void grpc_rdma_util_print_addr(const struct sockaddr *addr);

#endif /* GRPC_CORE_LIB_IOMGR_RMDA_VERBS_UTILS_POSIX_H */

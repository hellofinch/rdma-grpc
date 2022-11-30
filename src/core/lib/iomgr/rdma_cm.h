#include "src/core/lib/iomgr/endpoint.h"
#include "src/core/lib/iomgr/ev_posix.h"
#include "src/core/lib/iomgr/rdma_utils_posix.h"

//#define GRPC_TCP_DEFAULT_READ_SLICE_SIZE 2048

// extern int grpc_tcp_trace;

/* Create a tcp endpoint given a file desciptor and a read slice size.
   Takes ownership of fd. */
grpc_endpoint *grpc_rdma_create(connect_context* context,
                               const char *peer_string);

#define MSGINFO_MESSAGE 0
#define SENDCONTEXT_DATA 0
#define SENDCONTEXT_SMS 1
/* Return the tcp endpoint's fd, or -1 if this is not available. Does not
   release the fd.
   Requires: ep must be a tcp endpoint.
 */
int grpc_rdma_fd(grpc_endpoint *ep);

/* Destroy the tcp endpoint without closing its fd. *fd will be set and done
 * will be called when the endpoint is destroyed.
 * Requires: ep must be a tcp endpoint and fd must not be NULL. */
void grpc_rdma_destroy_and_release_fd(grpc_endpoint *ep,
                                      int* fd_in,int *fd_out, grpc_closure *done);
/* Since RDMA has no fd, we need to sentence it to death when the peer disconnected.
 * When an endpoint was sentenced to death, all the call_backs will be returned with an error.*/
void grpc_rdma_sentence_death(grpc_endpoint *ep);

// #endif /* GRPC_CORE_LIB_IOMGR_TCP_POSIX_H */

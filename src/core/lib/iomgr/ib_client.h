#ifndef GRPC_CORE_LIB_IOMGR_RDMA_CLIENT_H
#define GRPC_CORE_LIB_IOMGR_RDMA_CLIENT_H
/* modify from tcp_client.h*/
#include <grpc/support/port_platform.h>

#include <grpc/impl/codegen/grpc_types.h>
#include <grpc/support/time.h>

#include "src/core/lib/iomgr/endpoint.h"
#include "src/core/lib/iomgr/pollset_set.h"
#include "src/core/lib/iomgr/resolve_address.h"
#include "src/core/lib/resource_quota/memory_quota.h"

typedef struct grpc_rdma_client_vtable {
  void (*connect)(grpc_closure* on_connect, grpc_endpoint** endpoint,
                  grpc_pollset_set* interested_parties,
                  const grpc_channel_args* channel_args,
                  const grpc_resolved_address* addr,
                  grpc_core::Timestamp deadline);
} grpc_rdma_client_vtable;

/* Asynchronously connect to an address (specified as (addr, len)), and call
   cb with arg and the completed connection when done (or call cb with arg and
   NULL on failure).
   interested_parties points to a set of pollsets that would be interested
   in this connection being established (in order to continue their work) */
void grpc_rdma_client_connect(grpc_closure *closure,
    grpc_endpoint **ep,
    grpc_pollset_set *interested_parties,const grpc_channel_args* channel_args,
   const grpc_resolved_address* addr,
    grpc_core::Timestamp deadline);

void grpc_rdma_client_global_init();

void grpc_set_rdma_client_impl(grpc_rdma_client_vtable* impl);

#endif /* GRPC_CORE_LIB_IOMGR_RDMA_CLIENT_H */
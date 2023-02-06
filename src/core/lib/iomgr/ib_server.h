#ifndef GRPC_CORE_LIB_IOMGR_RDMA_SERVER_H
#define GRPC_CORE_LIB_IOMGR_RDMA_SERVER_H
/* modify from rdma_server.h */

#include <grpc/support/port_platform.h>

#include <grpc/grpc.h>
#include <grpc/impl/codegen/grpc_types.h>

#include "src/core/lib/iomgr/closure.h"
#include "src/core/lib/iomgr/endpoint.h"
#include "src/core/lib/iomgr/resolve_address.h"
// #include "src/core/lib/iomgr/tcp_server_utils_posix.h"

/* Forward decl of grpc_rdma_server */
typedef struct grpc_rdma_server grpc_rdma_server;

typedef struct grpc_rdma_server_acceptor {
  /* grpc_rdma_server_cb functions share a ref on from_server that is valid
     until the function returns. */
  grpc_rdma_server *from_server;
  /* Indices that may be passed to grpc_rdma_server_port_fd(). */
  unsigned port_index;
  unsigned fd_index;
  bool external_connection;
  int listener_fd;
  grpc_byte_buffer* pending_data;
} grpc_rdma_server_acceptor;

/* Called for newly connected rdma connections. */
typedef void (*grpc_rdma_server_cb)(void *arg,
                                   grpc_endpoint *ep,
                                   grpc_pollset *accepting_pollset,
                                   grpc_rdma_server_acceptor *acceptor);

typedef struct grpc_rdma_server_vtable {
  grpc_error_handle (*create)(grpc_closure* shutdown_complete,
                              const grpc_channel_args* args,
                              grpc_rdma_server** server);
  void (*start)(grpc_rdma_server* server,
                const std::vector<grpc_pollset*>* pollsets,
                grpc_rdma_server_cb on_accept_cb, void* cb_arg);
  grpc_error_handle (*add_port)(grpc_rdma_server* s,
                                const grpc_resolved_address* addr,
                                int* out_port);
   //   grpc_core::TcpServerFdHandler* (*create_fd_handler)(grpc_tcp_server* s);
  unsigned (*port_fd_count)(grpc_rdma_server* s, unsigned port_index);
  int (*port_fd)(grpc_rdma_server* s, unsigned port_index, unsigned fd_index);
  grpc_rdma_server* (*ref)(grpc_rdma_server* s);
  void (*shutdown_starting_add)(grpc_rdma_server* s,
                                grpc_closure* shutdown_starting);
  void (*unref)(grpc_rdma_server* s);
  void (*shutdown_listeners)(grpc_rdma_server* s);
} grpc_rdma_server_vtable;

/* Create a server, initially not bound to any ports. The caller owns one ref.
   If shutdown_complete is not NULL, it will be used by
   grpc_rdma_server_unref() when the ref count reaches zero. */
grpc_error_handle grpc_rdma_server_create(grpc_closure *shutdown_complete,
                                    const grpc_channel_args* args,
                                   grpc_rdma_server **server);

/* Start listening to bound ports */
void grpc_rdma_server_start( grpc_rdma_server *server,
                           const std::vector<grpc_pollset*>* pollsets,
                           grpc_rdma_server_cb on_accept_cb, void *cb_arg);

/* Add a port to the server, returning the newly allocated port on success, or
   -1 on failure.

   The :: and 0.0.0.0 wildcard addresses are treated identically, accepting
   both IPv4 and IPv6 connections, but :: is the preferred style.  This usually
   creates one socket, but possibly two on systems which support IPv6,
   but not dualstack sockets. */
/* TODO(ctiller): deprecate this, and make grpc_rdma_server_add_ports to handle
                  all of the multiple socket port matching logic in one place */
grpc_error_handle grpc_rdma_server_add_port(grpc_rdma_server *s, const grpc_resolved_address* addr, int *out_port);

/* Create and return a TcpServerFdHandler so that it can be used by upper layer
   to hand over an externally connected fd to the grpc server. */
// grpc_core::TcpServerFdHandler* grpc_rdma_server_create_fd_handler(
//     grpc_tcp_server* s);
/* Number of fds at the given port_index, or 0 if port_index is out of
   bounds. */
unsigned grpc_rdma_server_port_fd_count(grpc_rdma_server *s, unsigned port_index);

/* Returns the file descriptor of the Mth (fd_index) listening socket of the Nth
   (port_index) call to add_port() on this server, or -1 if the indices are out
   of bounds. The file descriptor remains owned by the server, and will be
   cleaned up when the ref count reaches zero. */
int grpc_rdma_server_port_fd(grpc_rdma_server *s, unsigned port_index,
                            unsigned fd_index);

/* Ref s and return s. */
grpc_rdma_server *grpc_rdma_server_ref(grpc_rdma_server *s);

/* shutdown_starting is called when ref count has reached zero and the server is
   about to be destroyed. The server will be deleted after it returns. Calling
   grpc_rdma_server_ref() from it has no effect. */
void grpc_rdma_server_shutdown_starting_add(grpc_rdma_server *s,
                                           grpc_closure *shutdown_starting);

/* If the refcount drops to zero, delete s, and call (exec_ctx==NULL) or enqueue
   a call (exec_ctx!=NULL) to shutdown_complete. */
void grpc_rdma_server_unref( grpc_rdma_server *s);

/* Shutdown the fds of listeners. */
void grpc_rdma_server_shutdown_listeners(grpc_rdma_server *s);

void grpc_rdma_server_global_init();

void grpc_set_rdma_server_impl(grpc_rdma_server_vtable* impl);

#endif /* GRPC_CORE_LIB_IOMGR_RDMA_SERVER_H */

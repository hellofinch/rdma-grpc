/*
 *
 * Copyright 2018 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <grpc/support/port_platform.h>

#include "src/core/lib/iomgr/ib_server.h"
#include "src/core/lib/iomgr/rdma_cm.h"
#include "src/core/lib/surface/api_trace.h"

grpc_rdma_server_vtable* grpc_rdma_server_impl;

grpc_error_handle grpc_rdma_server_create(grpc_closure* shutdown_complete,
                                         const grpc_channel_args* args,
                                         grpc_rdma_server** server) {
  GRPC_API_TRACE("src/core/lib/iomgr/ib_server.cc:grpc_rdma_server_create() begin",0, ());
  GRPC_API_TRACE("src/core/lib/iomgr/ib_server.cc:grpc_rdma_server_create() grpc_rdma_server_impl: %d",1, (grpc_rdma_server_impl));
  return grpc_rdma_server_impl->create(shutdown_complete, args, server);
}

void grpc_rdma_server_start(grpc_rdma_server* server,
                           const std::vector<grpc_pollset*>* pollsets,
                           grpc_rdma_server_cb on_accept_cb, void* cb_arg) {
  GRPC_API_TRACE("src/core/lib/iomgr/ib_server.cc:grpc_rdma_server_start()",0, ());
  // std::cout << "src/core/lib/iomgr/ib_server.cc:grpc_rdma_server_start()" << std::endl;
  grpc_rdma_server_impl->start(server, pollsets, on_accept_cb, cb_arg);
}

grpc_error_handle grpc_rdma_server_add_port(grpc_rdma_server* s,
                                           const grpc_resolved_address* addr,
                                           int* out_port) {
  GRPC_API_TRACE("src/core/lib/iomgr/ib_server.cc:grpc_rdma_server_add_port()",0, ());
  // std::cout << "src/core/lib/iomgr/ib_server.cc:grpc_rdma_server_add_port()" << std::endl;
  return grpc_rdma_server_impl->add_port(s, addr, out_port);
}

// grpc_core::TcpServerFdHandler* grpc_rdma_server_create_fd_handler(
//     grpc_tcp_server* s) {
//   return grpc_rdma_server_impl->create_fd_handler(s);
// }

unsigned grpc_rdma_server_port_fd_count(grpc_rdma_server* s,
                                       unsigned port_index) {
  GRPC_API_TRACE("src/core/lib/iomgr/ib_server.cc:grpc_rdma_server_port_fd_count()",0, ());
  // std::cout << "src/core/lib/iomgr/ib_server.cc:grpc_rdma_server_port_fd_count()" << std::endl;
  return grpc_rdma_server_impl->port_fd_count(s, port_index);
}

int grpc_rdma_server_port_fd(grpc_rdma_server* s, unsigned port_index,
                            unsigned fd_index) {
  GRPC_API_TRACE("src/core/lib/iomgr/ib_server.cc:grpc_rdma_server_port_fd()",0, ());
  // std::cout << "src/core/lib/iomgr/ib_server.cc:grpc_rdma_server_port_fd()" << std::endl;
  return grpc_rdma_server_impl->port_fd(s, port_index, fd_index);
}

grpc_rdma_server* grpc_rdma_server_ref(grpc_rdma_server* s) {
  GRPC_API_TRACE("src/core/lib/iomgr/ib_server.cc:grpc_rdma_server_ref()",0, ());
  // std::cout << "src/core/lib/iomgr/ib_server.cc:grpc_rdma_server_ref()" << std::endl;
  return grpc_rdma_server_impl->ref(s);
}

void grpc_rdma_server_shutdown_starting_add(grpc_rdma_server* s,
                                           grpc_closure* shutdown_starting) {
  GRPC_API_TRACE("src/core/lib/iomgr/ib_server.cc:grpc_rdma_server_shutdown_starting_add()",0, ());
  // std::cout << "src/core/lib/iomgr/ib_server.cc:grpc_rdma_server_shutdown_starting_add()" << std::endl;
  grpc_rdma_server_impl->shutdown_starting_add(s, shutdown_starting);
}

void grpc_rdma_server_unref(grpc_rdma_server* s) {
  GRPC_API_TRACE("src/core/lib/iomgr/ib_server.cc:grpc_rdma_server_unref()",0, ());
  // std::cout << "src/core/lib/iomgr/ib_server.cc:grpc_rdma_server_unref()" << std::endl;
  grpc_rdma_server_impl->unref(s);
}

void grpc_rdma_server_shutdown_listeners(grpc_rdma_server* s) {
  GRPC_API_TRACE("src/core/lib/iomgr/ib_server.cc:grpc_rdma_server_shutdown_listeners()",0, ());
  // std::cout << "src/core/lib/iomgr/ib_server.cc:grpc_rdma_server_shutdown_listeners()" << std::endl;
  grpc_rdma_server_impl->shutdown_listeners(s);
}

void grpc_set_rdma_server_impl(grpc_rdma_server_vtable* impl) {
  GRPC_API_TRACE("src/core/lib/iomgr/ib_server.cc:grpc_set_rdma_server_impl() begin",0, ());
  grpc_rdma_server_impl = impl;
}

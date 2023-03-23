#ifndef GRPC_RDMA_MEMORY_MANAGE_H
#define GRPC_RDMA_MEMORY_MANAGR_H
#define RDMA_MSG_CONTENT_SIZE 32767
#define INIT_RECV_BUFFER_SIZE (sizeof(rdma_memory_region))
#include <stddef.h>
#include "grpc/impl/codegen/slice.h"
#include "src/core/lib/slice/slice_refcount_base.h"
#include "grpc/impl/codegen/gpr_slice.h"
#include "grpc/impl/codegen/sync_generic.h"
#include "src/core/lib/gprpp/ref_counted.h"
typedef struct{
  int msg_info;
  size_t msg_len;
  char msg_content[RDMA_MSG_CONTENT_SIZE];
} rdma_message;
typedef struct{
  int msg_info;
  size_t msg_len;
  char msg_content[2];
} rdma_smessage;//In order to keep the same structure of rdma_message
typedef struct{
  rdma_message msg;
  rdma_smessage sms;
} rdma_memory_region;
typedef struct _rdma_mem_node rdma_mem_node;
typedef struct _rdma_mem_manager rdma_mem_manager;
struct _rdma_mem_node{
  rdma_memory_region context;
  struct ibv_mr *mr;
  rdma_mem_node *next,*prev;
  rdma_mem_manager *manager;
};
struct _rdma_mem_manager{
  rdma_mem_node *nil;
  grpc_core::RefCount refs;
  int node_count;//for debugging
};
rdma_mem_node *rdma_mm_pop(rdma_mem_manager* manager);
rdma_mem_node *rdma_mm_alloc_node(rdma_mem_manager* manager);
void rdma_mm_push(rdma_mem_node* tar);
//void rdma_mm_free(rdma_mem_manager* tar);
void rdma_mm_unref(rdma_mem_manager* tar);
rdma_mem_manager* rdma_mm_create();
grpc_slice rdma_mm_get_slice(rdma_message* buffer);
//Assume that buffer comes from a RDMA_MESSSAGE
#endif

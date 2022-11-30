#include "rdma_memory_management.h"
#include "grpc/support/alloc.h"
#include "grpc/support/log.h"
#include <rdma/rdma_cma.h>
#include "grpc/support/sync.h"
// #include "grpc/impl/codegen/gpr_slice.h"
rdma_mem_node* rdma_mm_alloc_node(rdma_mem_manager *manager){
	//gpr_log(GPR_DEBUG,"ALLOCATE NEW BUFFER");
	rdma_mem_node* ans=(rdma_mem_node*)gpr_malloc(sizeof(rdma_mem_node));
	ans->mr=NULL;
	ans->manager=manager;
	//gpr_ref(&manager->refs);
	return(ans);
}
rdma_mem_manager* rdma_mm_create(){
	rdma_mem_manager* tar=(rdma_mem_manager*)gpr_malloc(sizeof(rdma_mem_manager));
	tar->nil=rdma_mm_alloc_node(tar);
	tar->nil->next=tar->nil->prev=tar->nil;
	tar->node_count=0;
	gpr_ref_init(&tar->refs,1);
	return(tar);
}

typedef struct{
  gpr_slice_refcount base;
  gpr_refcount refs;
  rdma_mem_node* node;
}rdma_mm_refcounter;
static void rdma_mm_slice_ref(void* arg){
	rdma_mm_refcounter* rc=(rdma_mm_refcounter*)arg;
	gpr_ref(&rc->refs);
}
static void rdma_mm_slice_unref(void* arg){
	rdma_mm_refcounter* rc=(rdma_mm_refcounter*)arg;
	if(gpr_unref(&rc->refs)){
		rdma_mm_push(rc->node);
		rdma_mm_unref(rc->node->manager);
		gpr_free(rc);
	}
}
gpr_slice rdma_mm_get_slice(rdma_message* buffer){
	rdma_mem_node *node=(rdma_mem_node*)buffer;
	gpr_slice slice;
	rdma_mm_refcounter* rc=(rdma_mm_refcounter*)gpr_malloc(sizeof(rdma_mm_refcounter));
	rc->base.ref=rdma_mm_slice_ref;
	rc->base.unref=rdma_mm_slice_unref;
	gpr_ref_init(&rc->refs,1);
	rc->node=node;
	slice.refcount=&rc->base;
	slice.data.refcounted.bytes=(uint8_t *)buffer->msg_content;
	slice.data.refcounted.length=buffer->msg_len;
	gpr_ref(&node->manager->refs);
	//gpr_log(GPR_DEBUG,"SLICE LENGTH=%d",(int)buffer->msg_len);
	return(slice);
}
void rdma_mm_push(rdma_mem_node* tar){
	//gpr_log(GPR_DEBUG,"PUSH BUFFER");
	rdma_mem_manager *manager=tar->manager;
	tar->prev=manager->nil;
	tar->next=manager->nil->next;
	tar->prev->next=tar->next->prev=tar;
	++manager->node_count;
	//rdma_mm_unref(manager);
}
rdma_mem_node* rdma_mm_pop(rdma_mem_manager* tar){
	rdma_mem_node* ans;
	if(tar->nil->prev==tar->nil){
		ans=rdma_mm_alloc_node(tar);
	}else{
	//	gpr_ref(&tar->refs);
	//	gpr_log(GPR_DEBUG,"TAKE OUT OLD BUFFER");
		ans=tar->nil->prev;
		ans->next->prev=ans->prev;
		ans->prev->next=ans->next;
		--tar->node_count;
	}
	return(ans);
}
static void rdma_mm_free(rdma_mem_manager* tar){
	rdma_mem_node *now,*next;
	int cnt=0;
	for(now=tar->nil->next;now!=tar->nil;now=next){
		next=now->next;
		if(now->mr!=NULL)
		  ibv_dereg_mr(now->mr);
		gpr_free(now);
		++cnt;
	}
	gpr_free(tar->nil);
	gpr_free(tar);
	//gpr_log(GPR_DEBUG,"Free Buffer %d",cnt);
}
void rdma_mm_unref(rdma_mem_manager *tar){
	//gpr_log(GPR_DEBUG,"-1");
	if(gpr_unref(&tar->refs))
		rdma_mm_free(tar);
}

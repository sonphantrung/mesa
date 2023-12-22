/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AMDGPU_VIRTIO_PROTO_H
#define AMDGPU_VIRTIO_PROTO_H

#include <stdint.h>
#include "amdgpu.h"
#include "amdgpu_drm.h"

#include "virtio/vdrm/vdrm.h"

#define DEFINE_CAST(parent, child)                                             \
   static inline struct child *to_##child(const struct parent *x)              \
   {                                                                           \
      return (struct child *)x;                                                \
   }

enum amdgpu_ccmd {
   AMDGPU_CCMD_QUERY_INFO = 1,
   AMDGPU_CCMD_GEM_NEW,
   AMDGPU_CCMD_BO_VA_OP,
   AMDGPU_CCMD_CS_SUBMIT,
   AMDGPU_CCMD_SET_METADATA,
   AMDGPU_CCMD_BO_QUERY_INFO,
   AMDGPU_CCMD_CREATE_CTX,
   AMDGPU_CCMD_RESERVE_VMID,
   AMDGPU_CCMD_SET_PSTATE,
};

struct amdgpu_ccmd_rsp {
   struct vdrm_ccmd_rsp base;
   int32_t ret;
};


/**
 * Defines the layout of shmem buffer used for host->guest communication.
 */
struct amdvgpu_shmem {
   struct vdrm_shmem base;

   /**
    * Counter that is incremented on asynchronous errors, like SUBMIT
    * or GEM_NEW failures.  The guest should treat errors as context-
    * lost.
    */
   uint32_t async_error;

   uint32_t __pad;

   struct amdgpu_heap_info gtt;
   struct amdgpu_heap_info vram;
   struct amdgpu_heap_info vis_vram;
};
DEFINE_CAST(vdrm_shmem, amdvgpu_shmem)


#define AMDGPU_CCMD(_cmd, _len) (struct vdrm_ccmd_req){ \
       .cmd = AMDGPU_CCMD_##_cmd,                         \
       .len = (_len),                                     \
   }

/*
 * AMDGPU_CCMD_QUERY_INFO
 *
 * This is amdgpu_query_info.
 */
struct amdgpu_ccmd_query_info_req {
   struct vdrm_ccmd_req hdr;
   struct drm_amdgpu_info info;
};
DEFINE_CAST(vdrm_ccmd_req, amdgpu_ccmd_query_info_req)

struct amdgpu_ccmd_query_info_rsp {
   struct amdgpu_ccmd_rsp hdr;
   uint8_t payload[];
};

struct amdgpu_ccmd_gem_new_req {
   struct vdrm_ccmd_req hdr;

   uint64_t blob_id;
   uint64_t va;
   uint32_t pad;
   uint32_t vm_flags;
   uint64_t vm_map_size; /* may be smaller than alloc_size */

   /* This is amdgpu_bo_alloc_request but padded correctly. */
   struct {
      uint64_t alloc_size;
      uint64_t phys_alignment;
      uint32_t preferred_heap;
      uint32_t __pad;
      uint64_t flags;
   } r;
};
DEFINE_CAST(vdrm_ccmd_req, amdgpu_ccmd_gem_new_req)


/*
 * AMDGPU_CCMD_BO_VA_OP
 *
 */
struct amdgpu_ccmd_bo_va_op_req {
   struct vdrm_ccmd_req hdr;
   uint64_t va;
   uint64_t vm_map_size;
   uint64_t flags;
   uint64_t offset;
   uint32_t res_id;
   uint32_t op;
   bool is_sparse_bo;
};
DEFINE_CAST(vdrm_ccmd_req, amdgpu_ccmd_bo_va_op_req)

/*
 * AMDGPU_CCMD_CS_SUBMIT
 */
struct amdgpu_ccmd_cs_submit_req {
   struct vdrm_ccmd_req hdr;

   uint32_t ctx_id;
   uint32_t num_chunks;
   uint32_t bo_number;
   uint32_t ring_idx;

   /* Starts with a descriptor array:
    *     (chunk_id, offset_in_payload), ...
    */
   uint8_t payload[];
};
DEFINE_CAST(vdrm_ccmd_req, amdgpu_ccmd_cs_submit_req)

/*
 * AMDGPU_CCMD_SET_METADATA
 */
struct amdgpu_ccmd_set_metadata_req {
   struct vdrm_ccmd_req hdr;
   uint64_t flags;
   uint64_t tiling_info;
   uint32_t res_id;
   uint32_t size_metadata;
   uint32_t umd_metadata[];
};
DEFINE_CAST(vdrm_ccmd_req, amdgpu_ccmd_set_metadata_req)


/*
 * AMDGPU_CCMD_BO_QUERY_INFO
 */
struct amdgpu_ccmd_bo_query_info_req {
   struct vdrm_ccmd_req hdr;
   uint32_t res_id;
};
DEFINE_CAST(vdrm_ccmd_req, amdgpu_ccmd_bo_query_info_req)

struct amdgpu_ccmd_bo_query_info_rsp {
   struct amdgpu_ccmd_rsp hdr;
   uint32_t __pad;
   /* This is almost struct amdgpu_bo_info, but padded to get
    * the same struct on 32 bit and 64 bit builds.
    */
   struct {
      uint64_t alloc_size;           /*     0     8 */
      uint64_t phys_alignment;       /*     8     8 */
      uint32_t preferred_heap;       /*    16     4 */
      uint32_t __pad;
      uint64_t alloc_flags;          /*    20     8 */
      struct amdgpu_bo_metadata  metadata;
   } info;
};

/*
 * AMDGPU_CCMD_CREATE_CTX
 */
struct amdgpu_ccmd_create_ctx_req {
   struct vdrm_ccmd_req hdr;
   union {
      int32_t priority; /* create */
      uint32_t id;      /* destroy */
   };
   bool create;
};
DEFINE_CAST(vdrm_ccmd_req, amdgpu_ccmd_create_ctx_req)

struct amdgpu_ccmd_create_ctx_rsp {
   struct amdgpu_ccmd_rsp hdr;
   uint32_t ctx_id;
};

/*
 * AMDGPU_CCMD_RESERVE_VMID
 */
struct amdgpu_ccmd_reserve_vmid_req {
   struct vdrm_ccmd_req hdr;
   bool enable;
};
DEFINE_CAST(vdrm_ccmd_req, amdgpu_ccmd_reserve_vmid_req)

/*
 * AMDGPU_CCMD_SET_PSTATE
 */
struct amdgpu_ccmd_set_pstate_req {
   struct vdrm_ccmd_req hdr;
   uint32_t ctx_id;
   uint32_t op;
   uint32_t flags;
};
struct amdgpu_ccmd_set_pstate_rsp {
   struct amdgpu_ccmd_rsp hdr;
   uint32_t out_flags;
};
DEFINE_CAST(vdrm_ccmd_req, amdgpu_ccmd_set_pstate_req)

#endif

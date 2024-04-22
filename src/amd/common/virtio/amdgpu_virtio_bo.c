/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "drm-uapi/amdgpu_drm.h"

#include "amdgpu_virtio_private.h"

#include "util/list.h"
#include "util/log.h"
#include "util/os_mman.h"
#include "util/os_time.h"
#include "util/u_math.h"

#include <xf86drm.h>
#include <string.h>
#include <fcntl.h>

/* amdvgpu_host_blob stores info about a host blob.
 *
 * - a single amdvgpu_host_blob can be referenced by multiple
 * amdvgpu_bo (suballocation), hence the refcount.
 * - cpu mapping/unmapping is done at the host_blob level
 */
struct amdvgpu_host_blob {
   /* virtgpu properties */
   uint32_t handle;
   uint32_t res_id;
   uint64_t alloc_size;

   /* CPU mapping handling. */
   uint64_t offset;
   int map_count;
   void *cpu_addr;
   simple_mtx_t cpu_access_mutex;

   /* Allocation parameters. */
   uint32_t vm_flags;
   uint32_t preferred_heap;
   uint64_t phys_alignment;
   uint64_t flags;
};

static
void destroy_host_blob(amdvgpu_device_handle dev, struct amdvgpu_host_blob *hb);

static
struct amdvgpu_host_blob *create_host_blob(uint32_t kms_handle,
                                           uint32_t res_id,
                                           uint64_t size,
                                           struct amdgpu_ccmd_gem_new_req *req)
{
   struct amdvgpu_host_blob *hb = calloc(1, sizeof(*hb));
   hb->handle = kms_handle;
   hb->res_id = res_id;
   hb->alloc_size = size;

   if (req) {
      hb->vm_flags = req->vm_flags;
      hb->phys_alignment = req->r.phys_alignment;
      hb->preferred_heap = req->r.preferred_heap;
      hb->flags = req->r.flags;
   }

   simple_mtx_init(&hb->cpu_access_mutex, mtx_plain);
   return hb;
}

static
void destroy_host_blob(amdvgpu_device_handle dev, struct amdvgpu_host_blob *hb) {
   if (hb->cpu_addr) {
      int r;
      if (0 && hb->map_count) /* debug */
         mesa_logw("warn: unbalanced map/unmap (map_count:%d) for "
                   "host blob (res_id: %d)\n", hb->map_count, hb->res_id);
      r = os_munmap(hb->cpu_addr, hb->alloc_size);
      assert(r == 0);
   }
   simple_mtx_destroy(&hb->cpu_access_mutex);

   struct drm_gem_close req = {
      .handle = hb->handle,
   };
   int r = drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &req);
   if (r != 0) {
      mesa_loge("DRM_IOCTL_GEM_CLOSE failed for res_id: %d\n", hb->res_id);
   }

   free(hb);
}

static
struct amdvgpu_host_blob *alloc_host_blob(amdvgpu_bo_handle bo,
                                          struct drm_virtgpu_resource_create_blob *args,
                                          struct amdgpu_ccmd_gem_new_req *req,
                                          bool *va_map_done)
{
      uint32_t kms_handle, res_id;

      req->r.flags &= ~(1llu << 63);

      /* Create the host blob requires 2 steps. First create the host blob... */
      kms_handle = vdrm_bo_create(bo->dev->vdev, req->r.alloc_size, args->blob_flags,
                                  args->blob_id, &req->hdr);

      /* 0 is an invalid handle and is used by vdrm_bo_create to signal an error. */
      if (kms_handle == 0) {
         mesa_loge("VIRTGPU_RESOURCE_CREATE_BLOB failed (%s)\n", strerror(errno));
         mesa_loge("\targs.blob_mem:   %u\n", args->blob_mem);
         mesa_loge("\targs.blob_flags: %u\n", args->blob_flags);
         mesa_loge("\targs.bo_handle:  %u\n", args->bo_handle);
         mesa_loge("\targs.res_handle: %u\n", args->res_handle);
         mesa_loge("\targs.size:      %" PRIu64 "\n", req->r.alloc_size);
         mesa_loge("\targs.cmd_size:   %u\n", args->cmd_size);
         return NULL;
      }

      /* ... and then retrieve its resource id (global id). */
      res_id = vdrm_handle_to_res_id(bo->dev->vdev, kms_handle);

      bo->host_blob = create_host_blob(kms_handle, res_id, req->r.alloc_size, req);

      if (va_map_done)
         *va_map_done = req->va != 0;

      return bo->host_blob;
}

static int ensure_host_blob_exists(amdvgpu_device_handle dev,
                                   amdvgpu_bo_handle bo,
                                   bool *va_map_done)
{
   if (va_map_done)
      *va_map_done = false;

   if (bo->host_blob_creation_params == NULL)
      return 0;

   /* Create host blob now. */
   struct amdgpu_ccmd_gem_new_req req = bo->host_blob_creation_params->req;
   struct drm_virtgpu_resource_create_blob args = bo->host_blob_creation_params->args;

   bo->host_blob = alloc_host_blob(bo, &args, &req, va_map_done);
   if (bo->host_blob == NULL)
      return -1;

   free(bo->host_blob_creation_params);
   bo->host_blob_creation_params = NULL;

   if (bo->host_blob) {
      simple_mtx_lock(&dev->handle_to_vbo_mutex);
      _mesa_hash_table_insert(dev->handle_to_vbo, (void*)(intptr_t)bo->host_blob->handle, bo);
      simple_mtx_unlock(&dev->handle_to_vbo_mutex);

      return 0;
   } else {
      return -1;
   }
}

int amdvgpu_bo_export(amdvgpu_bo_handle bo,
                      enum amdgpu_bo_handle_type type,
                      uint32_t *shared_handle)
{
   /* This can happen for non GTT/VRAM buffers. */
   int r = ensure_host_blob_exists(bo->dev, bo, NULL);
   if (r)
      return r;

   bo->exported = true;

   switch (type) {
   case amdgpu_bo_handle_type_kms:
      /* Return the resource id as this handle is only going to be used
       * internally (AMDGPU_CHUNK_ID_BO_HANDLES mostly).
       */
      *shared_handle = amdvgpu_get_resource_id(bo);
      return 0;

   case amdgpu_bo_handle_type_dma_buf_fd:
      return drmPrimeHandleToFD(bo->dev->fd, bo->host_blob->handle, DRM_CLOEXEC | DRM_RDWR,
                                (int*)shared_handle);

   case amdgpu_bo_handle_type_kms_noimport:
      /* Treat this deprecated type as _type_kms and return the GEM handle. */
      *shared_handle = bo->host_blob->handle;
      return 0;

   case amdgpu_bo_handle_type_gem_flink_name:
      break;
   }
   return -EINVAL;
}

int amdvgpu_bo_free(struct amdvgpu_bo *bo) {
   int refcnt = p_atomic_dec_return(&bo->refcount);

   if (refcnt == 0) {
      /* Remove it from the bo table. */
      if (bo->host_blob->handle >= 0) {
         simple_mtx_lock(&bo->dev->handle_to_vbo_mutex);
         struct hash_entry *entry = _mesa_hash_table_search(bo->dev->handle_to_vbo,
                                                            (void*)(intptr_t)bo->host_blob->handle);
         if (entry) {
            /* entry can be NULL for the shmem buffer. */
            _mesa_hash_table_remove(bo->dev->handle_to_vbo, entry);
         }
         simple_mtx_unlock(&bo->dev->handle_to_vbo_mutex);
      }
   
      destroy_host_blob(bo->dev, bo->host_blob);
   
      free(bo);
   }

   return 0;
}

int amdvgpu_bo_alloc(amdvgpu_device_handle dev,
                     struct amdgpu_bo_alloc_request *request,
                     amdvgpu_bo_handle *bo)
{
   /* Use the same API as the one exposed by libdrm_amdgpu.
    * So we'll store the various parameters, and issue the request
    * at the very end.
    */
   struct drm_virtgpu_resource_create_blob args = {
         .blob_mem   = VIRTGPU_BLOB_MEM_HOST3D,
         .size       = request->alloc_size,
   };
   struct amdgpu_ccmd_gem_new_req req = {
         .hdr = AMDGPU_CCMD(GEM_NEW, sizeof(req)),
   };
   req.r.alloc_size = request->alloc_size;
   req.r.phys_alignment = request->phys_alignment;
   req.r.preferred_heap = request->preferred_heap;
   req.r.flags = request->flags;
   req.vm_map_size = request->alloc_size;

   if (!(request->flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS)) {
      args.blob_flags |= VIRTGPU_BLOB_FLAG_USE_MAPPABLE;
   }

   args.blob_id = p_atomic_inc_return(&dev->next_blob_id);
   req.blob_id = args.blob_id;
   /* blob_id 0 is reserved for the shared memory buffer. */
   assert(req.blob_id > 0);

   amdvgpu_bo_handle out = calloc(1, sizeof(struct amdvgpu_bo));
   out->dev = dev;
   out->size = request->alloc_size;
   p_atomic_set(&out->refcount, 1);
   *bo = out;

   /* Store the parameters for later. */
   out->host_blob_creation_params = calloc(1, sizeof(*out->host_blob_creation_params));
   out->host_blob_creation_params->args = args;
   out->host_blob_creation_params->req = req;

   return 0;
}

int amdvgpu_bo_va_op_raw(amdvgpu_device_handle dev,
                         amdvgpu_bo_handle bo,
                         uint64_t offset,
                         uint64_t size,
                         uint64_t addr,
                         uint64_t flags,
                         uint32_t ops)
{
   int r;

   /* Fill base structure fields. */
   struct amdgpu_ccmd_bo_va_op_req req = {
      .hdr = AMDGPU_CCMD(BO_VA_OP, sizeof(req)),
      .va = addr,
      .res_id = (bo && bo->host_blob) ? bo->host_blob->res_id : 0,
      .offset = offset + (bo ? bo->suballocation_offset : 0),
      .vm_map_size = size,
      .flags = flags,
      .op = ops,
      .is_sparse_bo = bo == NULL,
   };
   struct amdgpu_ccmd_rsp *rsp =
      vdrm_alloc_rsp(dev->vdev, &req.hdr, sizeof(*rsp));

   if (req.is_sparse_bo || ops == AMDGPU_VA_OP_UNMAP || ops == AMDGPU_VA_OP_REPLACE) {
      r = vdrm_send_req_wrapper(dev, &req.hdr, rsp, false);
   } else {
      assert (ops == AMDGPU_VA_OP_MAP);

      if (bo->host_blob_creation_params != NULL) {
         bool va_map_done;

         bo->host_blob_creation_params->req.vm_flags |= flags;
         /* VA_OP_MAP is done as part of the host blob effective creation. */
         bo->host_blob_creation_params->req.va = addr;

         r = ensure_host_blob_exists(dev, bo, &va_map_done);

         if (r == 0 && !va_map_done) {
            req.res_id = bo->host_blob->res_id;
            req.offset = offset + bo->suballocation_offset;
            r = vdrm_send_req_wrapper(dev, &req.hdr, rsp, false);
         }
      } else {
         r = vdrm_send_req_wrapper(dev, &req.hdr, rsp, false);
      }
   }

   return r;
}

int amdvgpu_bo_import(amdvgpu_device_handle dev, enum amdgpu_bo_handle_type type, uint32_t handle, struct amdgpu_bo_import_result *result)
{
   if (type != amdgpu_bo_handle_type_dma_buf_fd)
      return -1;

   uint32_t kms_handle;
   int r = drmPrimeFDToHandle(dev->fd, handle, &kms_handle);
   if (r) {
      mesa_loge("drmPrimeFDToHandle failed for dmabuf fd: %u\n", handle);
      return r;
   }

   /* Look up existing bo. */
   simple_mtx_lock(&dev->handle_to_vbo_mutex);
   struct hash_entry *entry = _mesa_hash_table_search(dev->handle_to_vbo, (void*)(intptr_t)kms_handle);

   if (entry) {
      struct amdvgpu_bo *bo = entry->data;
      p_atomic_inc(&bo->refcount);
      simple_mtx_unlock(&dev->handle_to_vbo_mutex);
      result->buf_handle = (void*)bo;
      result->alloc_size = bo->size;
      return 0;
   }
   simple_mtx_unlock(&dev->handle_to_vbo_mutex);

   /* lseek() to get bo size */
   int size = lseek(handle, 0, SEEK_END);
   lseek(handle, 0, SEEK_CUR);

   struct drm_virtgpu_resource_info args = {
         .bo_handle = kms_handle,
   };
   r = virtio_ioctl(dev->fd, VIRTGPU_RESOURCE_INFO, &args);

   if (r) {
      mesa_loge("VIRTGPU_RESOURCE_INFO failed (%s)\n", strerror(errno));
      return r;
   }

   struct amdvgpu_bo *bo = calloc(1, sizeof(struct amdvgpu_bo));
   bo->dev = dev;
   bo->size = size;
   bo->host_blob = create_host_blob(kms_handle, args.res_handle, size, NULL);
   bo->imported = true;
   p_atomic_set(&bo->refcount, 1);

   result->buf_handle = (void*)bo;
   result->alloc_size = bo->size;

   simple_mtx_lock(&dev->handle_to_vbo_mutex);
   _mesa_hash_table_insert(dev->handle_to_vbo, (void*)(intptr_t)bo->host_blob->handle, bo);
   simple_mtx_unlock(&dev->handle_to_vbo_mutex);

   return 0;
}

static int amdvgpu_get_offset(amdvgpu_bo_handle bo_handle)
{
   if (bo_handle->host_blob->offset)
      return 0;

   struct drm_virtgpu_map req = {
      .handle = bo_handle->host_blob->handle,
   };
   int ret = virtio_ioctl(bo_handle->dev->fd, VIRTGPU_MAP, &req);
   if (ret) {
      mesa_loge("amdvgpu_bo_map failed (%s) handle: %d\n",
              strerror(errno), bo_handle->host_blob->handle);
      return ret;
   }
   bo_handle->host_blob->offset = req.offset;

   return 0;
}

int amdvgpu_bo_cpu_map(amdvgpu_bo_handle bo_handle, void **cpu) {
   int r;

   r = ensure_host_blob_exists(bo_handle->dev, bo_handle, NULL);
   if (r)
      return r;

   simple_mtx_lock(&bo_handle->host_blob->cpu_access_mutex);

   if (bo_handle->host_blob->cpu_addr == NULL) {
      assert(bo_handle->host_blob->cpu_addr == NULL);
      r = amdvgpu_get_offset(bo_handle);
      if (r) {
         mesa_loge("get_offset failed\n");
         simple_mtx_unlock(&bo_handle->host_blob->cpu_access_mutex);
         return r;
      }

      /* Use *cpu as a fixed address hint from the caller. */
      bo_handle->host_blob->cpu_addr = os_mmap(*cpu, bo_handle->host_blob->alloc_size,
                                               PROT_READ | PROT_WRITE, MAP_SHARED,
                                               bo_handle->dev->fd,
                                               bo_handle->host_blob->offset);
   }

   assert(bo_handle->host_blob->cpu_addr != MAP_FAILED);
   *cpu = bo_handle->host_blob->cpu_addr +
          bo_handle->suballocation_offset;
   p_atomic_inc(&bo_handle->host_blob->map_count);

   simple_mtx_unlock(&bo_handle->host_blob->cpu_access_mutex);

   return *cpu == MAP_FAILED;
}

int amdvgpu_bo_cpu_unmap(amdvgpu_bo_handle bo) {
   int r = 0;

   simple_mtx_lock(&bo->host_blob->cpu_access_mutex);
   if (bo->host_blob->map_count == 0) {
      simple_mtx_unlock(&bo->host_blob->cpu_access_mutex);
      return 0;
   }
   assert(bo->host_blob->cpu_addr);
   if (p_atomic_dec_zero(&bo->host_blob->map_count)) {
      /* Skip unmap for recycled host blob. */
      /*
      r = os_munmap(bo->host_blob->cpu_addr, bo->host_blob->alloc_size);
      bo->host_blob->cpu_addr = NULL;
      */
   }
   simple_mtx_unlock(&bo->host_blob->cpu_access_mutex);

   return r;
}

uint32_t amdvgpu_get_resource_id(amdvgpu_bo_handle bo) {
   return bo->host_blob->res_id;
}

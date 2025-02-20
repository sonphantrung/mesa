/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "nvk_device_memory.h"

#include "nouveau_bo.h"

#include "nvk_device.h"
#include "nvk_entrypoints.h"
#include "nvk_image.h"
#include "nvk_physical_device.h"

#include "nv_push.h"
#include "util/u_atomic.h"

#include <inttypes.h>
#include <sys/mman.h>

#include "nvtypes.h"
#include "nv_push_cl90b5.h"

/* Supports opaque fd only */
const VkExternalMemoryProperties nvk_opaque_fd_mem_props = {
   .externalMemoryFeatures =
      VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
      VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
   .exportFromImportedHandleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
   .compatibleHandleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
};

/* Supports opaque fd and dma_buf. */
const VkExternalMemoryProperties nvk_dma_buf_mem_props = {
   .externalMemoryFeatures =
      VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
      VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
   .exportFromImportedHandleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   .compatibleHandleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
};

static enum nouveau_ws_bo_flags
nvk_memory_type_flags(const VkMemoryType *type,
                      VkExternalMemoryHandleTypeFlagBits handle_types)
{
   enum nouveau_ws_bo_flags flags = 0;
   if (type->propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
      flags = NOUVEAU_WS_BO_LOCAL;
   else
      flags = NOUVEAU_WS_BO_GART;

   if (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      flags |= NOUVEAU_WS_BO_MAP;

   /* For dma-bufs, we have to allow them to live in GART because they might
    * get forced there by the kernel if they're shared with another GPU.
    */
   if (handle_types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
      flags |= NOUVEAU_WS_BO_GART;

   if (handle_types == 0)
      flags |= NOUVEAU_WS_BO_NO_SHARE;

   return flags;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_GetMemoryFdPropertiesKHR(VkDevice device,
                             VkExternalMemoryHandleTypeFlagBits handleType,
                             int fd,
                             VkMemoryFdPropertiesKHR *pMemoryFdProperties)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   struct nvk_physical_device *pdev = nvk_device_physical(dev);
   struct nouveau_ws_bo *bo;

   switch (handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
      bo = nouveau_ws_bo_from_dma_buf(dev->ws_dev, fd);
      if (bo == NULL)
         return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      break;
   default:
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }

   uint32_t type_bits = 0;
   if (handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
      /* We allow a dma-buf to be imported anywhere because there's no way
       * for us to actually know where it came from.
       */
      type_bits = BITFIELD_MASK(pdev->mem_type_count);
   } else {
      for (unsigned t = 0; t < ARRAY_SIZE(pdev->mem_types); t++) {
         const enum nouveau_ws_bo_flags flags =
            nvk_memory_type_flags(&pdev->mem_types[t], handleType);
         if (!(flags & ~bo->flags))
            type_bits |= (1 << t);
      }
   }

   pMemoryFdProperties->memoryTypeBits = type_bits;

   nouveau_ws_bo_destroy(bo);

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_AllocateMemory(VkDevice device,
                   const VkMemoryAllocateInfo *pAllocateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkDeviceMemory *pMem)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   struct nvk_physical_device *pdev = nvk_device_physical(dev);
   struct nvk_device_memory *mem;
   VkResult result = VK_SUCCESS;

   const VkImportMemoryFdInfoKHR *fd_info =
      vk_find_struct_const(pAllocateInfo->pNext, IMPORT_MEMORY_FD_INFO_KHR);
   const VkExportMemoryAllocateInfo *export_info =
      vk_find_struct_const(pAllocateInfo->pNext, EXPORT_MEMORY_ALLOCATE_INFO);
   const VkMemoryDedicatedAllocateInfo *dedicated_info =
      vk_find_struct_const(pAllocateInfo->pNext, MEMORY_DEDICATED_ALLOCATE_INFO);
   const VkMemoryType *type =
      &pdev->mem_types[pAllocateInfo->memoryTypeIndex];

   VkExternalMemoryHandleTypeFlagBits handle_types = 0;
   if (export_info != NULL)
      handle_types |= export_info->handleTypes;
   if (fd_info != NULL)
      handle_types |= fd_info->handleType;

   const enum nouveau_ws_bo_flags flags =
      nvk_memory_type_flags(type, handle_types);

   uint32_t alignment = (1ULL << 12);
   if (flags & NOUVEAU_WS_BO_LOCAL)
      alignment = (1ULL << 16);

   uint8_t pte_kind = 0, tile_mode = 0;
   if (dedicated_info != NULL) {
      VK_FROM_HANDLE(nvk_image, image, dedicated_info->image);
      if (image != NULL &&
          image->vk.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
         /* This image might be shared with GL so we need to set the BO flags
          * such that GL can bind and use it.
          */
         assert(image->plane_count == 1);
         alignment = MAX2(alignment, image->planes[0].nil.align_B);
         pte_kind = image->planes[0].nil.pte_kind;
         tile_mode = image->planes[0].nil.tile_mode;
      }
   }

   const uint64_t aligned_size =
      align64(pAllocateInfo->allocationSize, alignment);

   mem = vk_device_memory_create(&dev->vk, pAllocateInfo,
                                 pAllocator, sizeof(*mem));
   if (!mem)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   mem->map = NULL;
   if (fd_info && fd_info->handleType) {
      assert(fd_info->handleType ==
               VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
             fd_info->handleType ==
               VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

      mem->bo = nouveau_ws_bo_from_dma_buf(dev->ws_dev, fd_info->fd);
      if (mem->bo == NULL) {
         result = vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
         goto fail_alloc;
      }

      /* We can't really assert anything for dma-bufs because they could come
       * in from some other device.
       */
      if (fd_info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)
         assert(!(flags & ~mem->bo->flags));
   } else if (pte_kind != 0 || tile_mode != 0) {
      mem->bo = nouveau_ws_bo_new_tiled(dev->ws_dev, aligned_size, alignment,
                                        pte_kind, tile_mode, flags);
      if (!mem->bo) {
         result = vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY, "%m");
         goto fail_alloc;
      }
   } else {
      mem->bo = nouveau_ws_bo_new(dev->ws_dev, aligned_size, alignment, flags);
      if (!mem->bo) {
         result = vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         goto fail_alloc;
      }
   }

   if (dev->ws_dev->debug_flags & NVK_DEBUG_ZERO_MEMORY) {
      if (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
         void *map = nouveau_ws_bo_map(mem->bo, NOUVEAU_WS_BO_RDWR, NULL);
         if (map == NULL) {
            result = vk_errorf(dev, VK_ERROR_OUT_OF_HOST_MEMORY,
                               "Memory map failed");
            goto fail_bo;
         }
         memset(map, 0, mem->bo->size);
         nouveau_ws_bo_unmap(mem->bo, map);
      } else {
         result = nvk_upload_queue_fill(dev, &dev->upload,
                                        mem->bo->offset, 0, mem->bo->size);
         if (result != VK_SUCCESS)
            goto fail_bo;

         /* Since we don't know when the memory will be freed, sync now */
         result = nvk_upload_queue_sync(dev, &dev->upload);
         if (result != VK_SUCCESS)
            goto fail_bo;
      }
   }

   if (fd_info && fd_info->handleType) {
      /* From the Vulkan spec:
       *
       *    "Importing memory from a file descriptor transfers ownership of
       *    the file descriptor from the application to the Vulkan
       *    implementation. The application must not perform any operations on
       *    the file descriptor after a successful import."
       *
       * If the import fails, we leave the file descriptor open.
       */
      close(fd_info->fd);
   }

   struct nvk_memory_heap *heap = &pdev->mem_heaps[type->heapIndex];
   p_atomic_add(&heap->used, mem->bo->size);

   *pMem = nvk_device_memory_to_handle(mem);

   return VK_SUCCESS;

fail_bo:
   nouveau_ws_bo_destroy(mem->bo);
fail_alloc:
   vk_device_memory_destroy(&dev->vk, pAllocator, &mem->vk);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
nvk_FreeMemory(VkDevice device,
               VkDeviceMemory _mem,
               const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   VK_FROM_HANDLE(nvk_device_memory, mem, _mem);
   struct nvk_physical_device *pdev = nvk_device_physical(dev);

   if (!mem)
      return;

   if (mem->map)
      nouveau_ws_bo_unmap(mem->bo, mem->map);

   const VkMemoryType *type = &pdev->mem_types[mem->vk.memory_type_index];
   struct nvk_memory_heap *heap = &pdev->mem_heaps[type->heapIndex];
   p_atomic_add(&heap->used, -((int64_t)mem->bo->size));

   nouveau_ws_bo_destroy(mem->bo);

   vk_device_memory_destroy(&dev->vk, pAllocator, &mem->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_MapMemory2KHR(VkDevice device,
                  const VkMemoryMapInfoKHR *pMemoryMapInfo,
                  void **ppData)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   VK_FROM_HANDLE(nvk_device_memory, mem, pMemoryMapInfo->memory);

   if (mem == NULL) {
      *ppData = NULL;
      return VK_SUCCESS;
   }

   const VkDeviceSize offset = pMemoryMapInfo->offset;
   const VkDeviceSize size =
      vk_device_memory_range(&mem->vk, pMemoryMapInfo->offset,
                                       pMemoryMapInfo->size);

   void *fixed_addr = NULL;
   if (pMemoryMapInfo->flags & VK_MEMORY_MAP_PLACED_BIT_EXT) {
      const VkMemoryMapPlacedInfoEXT *placed_info =
         vk_find_struct_const(pMemoryMapInfo->pNext, MEMORY_MAP_PLACED_INFO_EXT);
      fixed_addr = placed_info->pPlacedAddress;
   }

   /* From the Vulkan spec version 1.0.32 docs for MapMemory:
    *
    *  * If size is not equal to VK_WHOLE_SIZE, size must be greater than 0
    *    assert(size != 0);
    *  * If size is not equal to VK_WHOLE_SIZE, size must be less than or
    *    equal to the size of the memory minus offset
    */
   assert(size > 0);
   assert(offset + size <= mem->bo->size);

   if (size != (size_t)size) {
      return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED,
                       "requested size 0x%"PRIx64" does not fit in %u bits",
                       size, (unsigned)(sizeof(size_t) * 8));
   }

   /* From the Vulkan 1.2.194 spec:
    *
    *    "memory must not be currently host mapped"
    */
   if (mem->map != NULL) {
      return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED,
                       "Memory object already mapped.");
   }

   mem->map = nouveau_ws_bo_map(mem->bo, NOUVEAU_WS_BO_RDWR, fixed_addr);
   if (mem->map == NULL) {
      return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED,
                       "Memory object couldn't be mapped.");
   }

   *ppData = mem->map + offset;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_UnmapMemory2KHR(VkDevice device,
                    const VkMemoryUnmapInfoKHR *pMemoryUnmapInfo)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   VK_FROM_HANDLE(nvk_device_memory, mem, pMemoryUnmapInfo->memory);

   if (mem == NULL)
      return VK_SUCCESS;

   if (pMemoryUnmapInfo->flags & VK_MEMORY_UNMAP_RESERVE_BIT_EXT) {
      int err = nouveau_ws_bo_overmap(mem->bo, mem->map);
      if (err) {
         return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED,
                          "Failed to map over original mapping");
      }
   } else {
      nouveau_ws_bo_unmap(mem->bo, mem->map);
   }

   mem->map = NULL;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_FlushMappedMemoryRanges(VkDevice device,
                            uint32_t memoryRangeCount,
                            const VkMappedMemoryRange *pMemoryRanges)
{
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_InvalidateMappedMemoryRanges(VkDevice device,
                                 uint32_t memoryRangeCount,
                                 const VkMappedMemoryRange *pMemoryRanges)
{
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetDeviceMemoryCommitment(VkDevice device,
                              VkDeviceMemory _mem,
                              VkDeviceSize* pCommittedMemoryInBytes)
{
   VK_FROM_HANDLE(nvk_device_memory, mem, _mem);

   *pCommittedMemoryInBytes = mem->bo->size;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_GetMemoryFdKHR(VkDevice device,
                   const VkMemoryGetFdInfoKHR *pGetFdInfo,
                   int *pFD)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   VK_FROM_HANDLE(nvk_device_memory, memory, pGetFdInfo->memory);

   switch (pGetFdInfo->handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
      if (nouveau_ws_bo_dma_buf(memory->bo, pFD))
         return vk_errorf(dev, VK_ERROR_TOO_MANY_OBJECTS,
                          "Failed to export dma-buf: %m");
      return VK_SUCCESS;
   default:
      assert(!"unsupported handle type");
      return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
   }
}

VKAPI_ATTR uint64_t VKAPI_CALL
nvk_GetDeviceMemoryOpaqueCaptureAddress(
   UNUSED VkDevice device,
   const VkDeviceMemoryOpaqueCaptureAddressInfo* pInfo)
{
   VK_FROM_HANDLE(nvk_device_memory, mem, pInfo->memory);

   return mem->bo->offset;
}

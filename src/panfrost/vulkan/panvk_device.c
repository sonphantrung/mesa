/*
 * Copyright © 2021 Collabora Ltd.
 *
 * Derived from tu_device.c which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "panvk_device.h"
#include "panvk_device_memory.h"
#include "panvk_entrypoints.h"
#include "panvk_image.h"
#include "panvk_instance.h"
#include "panvk_physical_device.h"
#include "panvk_queue.h"

#include "decode.h"

#include "pan_encoder.h"
#include "pan_util.h"
#include "pan_props.h"
#include "pan_samples.h"

#include "vk_cmd_enqueue_entrypoints.h"
#include "vk_common_entrypoints.h"

#include <fcntl.h>
#include <libsync.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>

#include "drm-uapi/panfrost_drm.h"

#include "util/disk_cache.h"
#include "util/strtod.h"
#include "util/u_debug.h"
#include "vk_drm_syncobj.h"
#include "vk_format.h"
#include "vk_util.h"

#ifdef VK_USE_PLATFORM_WAYLAND_KHR
#include "wayland-drm-client-protocol.h"
#include <wayland-client.h>
#endif


/* Always reserve the lower 32MB. */
#define PANVK_VA_RESERVE_BOTTOM 0x2000000ull

#define DEVICE_PER_ARCH_FUNCS(_ver)                                            \
   VkResult panvk_v##_ver##_create_device(                                     \
      struct panvk_physical_device *physical_device,                           \
      const VkDeviceCreateInfo *pCreateInfo,                                   \
      const VkAllocationCallbacks *pAllocator, VkDevice *pDevice);             \
                                                                               \
   void panvk_v##_ver##_destroy_device(                                        \
      struct panvk_device *device, const VkAllocationCallbacks *pAllocator)

DEVICE_PER_ARCH_FUNCS(6);
DEVICE_PER_ARCH_FUNCS(7);

VKAPI_ATTR VkResult VKAPI_CALL
panvk_CreateDevice(VkPhysicalDevice physicalDevice,
                   const VkDeviceCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
   VK_FROM_HANDLE(panvk_physical_device, physical_device, physicalDevice);
   unsigned arch = pan_arch(physical_device->kmod.props.gpu_prod_id);
   VkResult result = VK_ERROR_INITIALIZATION_FAILED;

   panvk_arch_dispatch_ret(arch, create_device, result, physical_device,
                           pCreateInfo, pAllocator, pDevice);

   return result;
}

VKAPI_ATTR void VKAPI_CALL
panvk_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_physical_device *physical_device =
      panvk_device_get_physical_device(device);
   unsigned arch = pan_arch(physical_device->kmod.props.gpu_prod_id);

   panvk_arch_dispatch(arch, destroy_device, device, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL
panvk_GetImageMemoryRequirements2(VkDevice device,
                                  const VkImageMemoryRequirementsInfo2 *pInfo,
                                  VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(panvk_image, image, pInfo->image);

   const uint64_t alignment = 4096;
   const uint64_t size = panvk_image_get_total_size(image);

   pMemoryRequirements->memoryRequirements.memoryTypeBits = 1;
   pMemoryRequirements->memoryRequirements.alignment = alignment;
   pMemoryRequirements->memoryRequirements.size = size;
}

VKAPI_ATTR void VKAPI_CALL
panvk_GetImageSparseMemoryRequirements2(
   VkDevice device, const VkImageSparseMemoryRequirementsInfo2 *pInfo,
   uint32_t *pSparseMemoryRequirementCount,
   VkSparseImageMemoryRequirements2 *pSparseMemoryRequirements)
{
   panvk_stub();
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_BindImageMemory2(VkDevice device, uint32_t bindInfoCount,
                       const VkBindImageMemoryInfo *pBindInfos)
{
   for (uint32_t i = 0; i < bindInfoCount; ++i) {
      VK_FROM_HANDLE(panvk_image, image, pBindInfos[i].image);
      VK_FROM_HANDLE(panvk_device_memory, mem, pBindInfos[i].memory);
      struct pan_kmod_bo *old_bo = image->bo;

      if (mem) {
         image->bo = pan_kmod_bo_get(mem->bo);
         image->pimage.data.base = mem->addr.dev;
         image->pimage.data.offset = pBindInfos[i].memoryOffset;
         /* Reset the AFBC headers */
         if (drm_is_afbc(image->pimage.layout.modifier)) {
            /* Transient CPU mapping */
            void *base = pan_kmod_bo_mmap(mem->bo, 0, pan_kmod_bo_size(mem->bo),
                                          PROT_WRITE, MAP_SHARED, NULL);

            assert(base != MAP_FAILED);

            for (unsigned layer = 0; layer < image->pimage.layout.array_size;
                 layer++) {
               for (unsigned level = 0; level < image->pimage.layout.nr_slices;
                    level++) {
                  void *header = base + image->pimage.data.offset +
                                 (layer * image->pimage.layout.array_stride) +
                                 image->pimage.layout.slices[level].offset;
                  memset(header, 0,
                         image->pimage.layout.slices[level].afbc.header_size);
               }
            }

            ASSERTED int ret = os_munmap(base, pan_kmod_bo_size(mem->bo));
            assert(!ret);
         }
      } else {
         image->bo = NULL;
         image->pimage.data.offset = pBindInfos[i].memoryOffset;
      }

      pan_kmod_bo_put(old_bo);
   }

   return VK_SUCCESS;
}

/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_DEVICE_H
#define PANVK_DEVICE_H

#include <stdint.h>

#include "vulkan/runtime/vk_device.h"

#include "panvk_instance.h"
#include "panvk_macros.h"
#include "panvk_mempool.h"
#include "panvk_meta.h"
#include "panvk_physical_device.h"

#include "kmod/pan_kmod.h"
#include "pan_blend.h"
#include "pan_blitter.h"
#include "util/pan_ir.h"

#define PANVK_MAX_QUEUE_FAMILIES 1

struct panvk_device {
   struct vk_device vk;

   struct {
      struct pan_kmod_vm *vm;
      struct pan_kmod_dev *dev;
      struct pan_kmod_allocator allocator;
   } kmod;

   struct panvk_priv_bo *tiler_heap;
   struct panvk_priv_bo *sample_positions;

   struct panvk_meta meta;

   struct vk_device_dispatch_table cmd_dispatch;

   struct panvk_queue *queues[PANVK_MAX_QUEUE_FAMILIES];
   int queue_count[PANVK_MAX_QUEUE_FAMILIES];

   struct {
      struct pandecode_context *decode_ctx;
   } debug;
};

VK_DEFINE_HANDLE_CASTS(panvk_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)

static inline struct panvk_physical_device *
panvk_device_get_physical_device(const struct panvk_device *dev)
{
   return container_of(dev->vk.physical, struct panvk_physical_device, vk);
}

static inline struct panvk_instance *
panvk_device_get_instance(const struct panvk_device *dev)
{
   struct panvk_physical_device *phys_dev =
      panvk_device_get_physical_device(dev);

   return panvk_physical_device_get_instance(phys_dev);
}

#if PAN_ARCH
VkResult
panvk_per_arch(create_device)(struct panvk_physical_device *physical_device,
                              const VkDeviceCreateInfo *pCreateInfo,
                              const VkAllocationCallbacks *pAllocator,
                              VkDevice *pDevice);

void
panvk_per_arch(destroy_device)(struct panvk_device *device,
                               const VkAllocationCallbacks *pAllocator);
#endif

#endif

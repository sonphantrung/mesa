/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_QUEUE_H
#define PANVK_QUEUE_H

#include <stdint.h>

#include "vulkan/runtime/vk_queue.h"

struct panvk_queue {
   struct vk_queue vk;
   struct panvk_device *device;
   uint32_t sync;
};

VK_DEFINE_HANDLE_CASTS(panvk_queue, vk.base, VkQueue, VK_OBJECT_TYPE_QUEUE)

static inline void
panvk_queue_finish(struct panvk_queue *queue)
{
   struct panvk_device *dev = queue->device;

   vk_queue_finish(&queue->vk);
   drmSyncobjDestroy(dev->vk.drm_fd, queue->sync);
}

#ifdef PAN_ARCH
VkResult
panvk_per_arch(queue_init)(struct panvk_device *device,
                           struct panvk_queue *queue, int idx,
                           const VkDeviceQueueCreateInfo *create_info);
#endif

#endif

/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_PHYSICAL_DEVICE_H
#define PANVK_PHYSICAL_DEVICE_H

#include <stdint.h>

#include "vulkan/runtime/vk_physical_device.h"
#include "vulkan/runtime/vk_sync.h"
#include "vulkan/wsi/wsi_common.h"

#include "lib/kmod/pan_kmod.h"

struct panfrost_model;
struct pan_blendable_format;
struct panfrost_format;
struct panvk_instance;

struct panvk_physical_device {
   struct vk_physical_device vk;

   struct {
      struct pan_kmod_dev *dev;
      struct pan_kmod_dev_props props;
   } kmod;

   const struct panfrost_model *model;
   struct {
      const struct pan_blendable_format *blendable;
      const struct panfrost_format *all;
   } formats;

   struct panvk_instance *instance;

   char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
   uint8_t driver_uuid[VK_UUID_SIZE];
   uint8_t device_uuid[VK_UUID_SIZE];
   uint8_t cache_uuid[VK_UUID_SIZE];

   struct vk_sync_type drm_syncobj_type;
   const struct vk_sync_type *sync_types[2];

   struct wsi_device wsi_device;

   int master_fd;
};

VK_DEFINE_HANDLE_CASTS(panvk_physical_device, vk.base, VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)

VkResult panvk_physical_device_init(struct panvk_physical_device *device,
                                    struct panvk_instance *instance,
                                    drmDevicePtr drm_device);

void panvk_physical_device_finish(struct panvk_physical_device *device);

#endif

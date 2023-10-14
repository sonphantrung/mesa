/*
 * Mesa 3-D graphics library
 *
 * Copyright © 2017, Google Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "panvk_private.h"

#include <hardware/hardware.h>
#include <hardware/hwvulkan.h>
#include <vulkan/vk_icd.h>

#include "util/log.h"
#include "util/u_gralloc/u_gralloc.h"
#include "vk_android.h"

static int panvk_hal_open(const struct hw_module_t *mod, const char *id,
                          struct hw_device_t **dev);
static int panvk_hal_close(struct hw_device_t *dev);

static_assert(HWVULKAN_DISPATCH_MAGIC == ICD_LOADER_MAGIC, "");

PUBLIC struct hwvulkan_module_t HAL_MODULE_INFO_SYM = {
   .common =
      {
         .tag = HARDWARE_MODULE_TAG,
         .module_api_version = HWVULKAN_MODULE_API_VERSION_0_1,
         .hal_api_version = HARDWARE_MAKE_API_VERSION(1, 0),
         .id = HWVULKAN_HARDWARE_MODULE_ID,
         .name = "ARM Vulkan HAL",
         .author = "Mesa3D",
         .methods =
            &(hw_module_methods_t){
               .open = panvk_hal_open,
            },
      },
};

static int
panvk_hal_open(const struct hw_module_t *mod, const char *id,
               struct hw_device_t **dev)
{
   assert(mod == &HAL_MODULE_INFO_SYM.common);
   assert(strcmp(id, HWVULKAN_DEVICE_0) == 0);

   hwvulkan_device_t *hal_dev = malloc(sizeof(*hal_dev));
   if (!hal_dev)
      return -1;

   *hal_dev = (hwvulkan_device_t){
      .common =
         {
            .tag = HARDWARE_DEVICE_TAG,
            .version = HWVULKAN_DEVICE_API_VERSION_0_1,
            .module = &HAL_MODULE_INFO_SYM.common,
            .close = panvk_hal_close,
         },
      .EnumerateInstanceExtensionProperties =
         panvk_EnumerateInstanceExtensionProperties,
      .CreateInstance = panvk_CreateInstance,
      .GetInstanceProcAddr = panvk_GetInstanceProcAddr,
   };

   mesa_logi("panvk: Warning: Android Vulkan implementation is experimental");

   struct u_gralloc *u_gralloc = u_gralloc_create(U_GRALLOC_TYPE_AUTO);

   if (u_gralloc && u_gralloc_get_type(u_gralloc) == U_GRALLOC_TYPE_FALLBACK) {
      mesa_logw(
         "panvk: Gralloc is not supported. Android extensions are disabled.");
      u_gralloc_destroy(&u_gralloc);
   }

   *vk_android_get_ugralloc_ptr() = u_gralloc;

   *dev = &hal_dev->common;
   return 0;
}

static int
panvk_hal_close(struct hw_device_t *dev)
{
   /* hwvulkan.h claims that hw_device_t::close() is never called. */
   u_gralloc_destroy(vk_android_get_ugralloc_ptr());
   return -1;
}

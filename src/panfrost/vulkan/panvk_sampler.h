/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_SAMPLER_H
#define PANVK_SAMPLER_H

#include <stdint.h>

#include "vulkan/runtime/vk_sampler.h"

struct panvk_sampler {
   struct vk_sampler vk;

   /* The PAN_ARCH section must stay at the end of the struct. */
#ifdef PAN_ARCH
   struct mali_sampler_packed desc;
#endif
};

VK_DEFINE_NONDISP_HANDLE_CASTS(panvk_sampler, vk.base, VkSampler,
                               VK_OBJECT_TYPE_SAMPLER)

#endif

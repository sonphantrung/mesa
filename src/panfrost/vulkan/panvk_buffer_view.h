/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_BUFFER_VIEW_H
#define PANVK_BUFFER_VIEW_H

#include <stdint.h>

#include "util/format/u_formats.h"

#include "vulkan/runtime/vk_object.h"

struct panvk_priv_bo;

#define TEXTURE_DESC_WORDS    8
#define ATTRIB_BUF_DESC_WORDS 4

struct panvk_buffer_view {
   struct vk_object_base base;
   struct panvk_priv_bo *bo;
   struct {
      uint32_t tex[TEXTURE_DESC_WORDS];
      uint32_t img_attrib_buf[ATTRIB_BUF_DESC_WORDS * 2];
   } descs;
   enum pipe_format fmt;
   uint32_t elems;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(panvk_buffer_view, base, VkBufferView,
                               VK_OBJECT_TYPE_BUFFER_VIEW)

#endif

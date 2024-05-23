/*
 * Copyright © 2024 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_CMD_DESC_STATE_H
#define PANVK_CMD_DESC_STATE_H

#ifndef PAN_ARCH
#error "PAN_ARCH must be defined"
#endif

#include <stdint.h>

#include "jm/panvk_cmd_buffer.h"
#include "panvk_descriptor_set.h"
#include "panvk_pipeline_layout.h"

#include "genxml/gen_macros.h"

struct panvk_descriptor_state {
   const struct panvk_descriptor_set *sets[MAX_SETS];
   struct panvk_push_descriptor_set *push_sets[MAX_SETS];

   struct {
      struct mali_uniform_buffer_packed ubos[MAX_DYNAMIC_UNIFORM_BUFFERS];
      struct panvk_ssbo_addr ssbos[MAX_DYNAMIC_STORAGE_BUFFERS];
   } dyn;
   mali_ptr ubos;
   mali_ptr textures;
   mali_ptr samplers;
   mali_ptr dyn_desc_ubo;
   mali_ptr push_uniforms;

   struct {
      mali_ptr attribs;
      mali_ptr attrib_bufs;
   } img;
};

void panvk_per_arch(cmd_desc_state_reset)(struct panvk_cmd_buffer *cmdbuf);
void panvk_per_arch(cmd_desc_state_cleanup)(struct panvk_cmd_buffer *cmdbuf);

#endif

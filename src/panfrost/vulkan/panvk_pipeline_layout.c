/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "panvk_pipeline_layout.h"
#include "panvk_shader.h"

unsigned
panvk_pipeline_layout_ubo_start(const struct panvk_pipeline_layout *layout,
                                unsigned set, bool is_dynamic)
{
   const struct panvk_descriptor_set_layout *set_layout =
      vk_to_panvk_descriptor_set_layout(layout->vk.set_layouts[set]);

   unsigned offset = PANVK_NUM_BUILTIN_UBOS + layout->sets[set].ubo_offset +
                     layout->sets[set].dyn_ubo_offset;

   if (is_dynamic)
      offset += set_layout->num_ubos;

   return offset;
}

unsigned
panvk_pipeline_layout_ubo_index(const struct panvk_pipeline_layout *layout,
                                unsigned set, unsigned binding,
                                unsigned array_index)
{
   const struct panvk_descriptor_set_layout *set_layout =
      vk_to_panvk_descriptor_set_layout(layout->vk.set_layouts[set]);
   const struct panvk_descriptor_set_binding_layout *binding_layout =
      &set_layout->bindings[binding];

   const bool is_dynamic =
      binding_layout->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
   const uint32_t ubo_idx =
      is_dynamic ? binding_layout->dyn_ubo_idx : binding_layout->ubo_idx;

   return panvk_pipeline_layout_ubo_start(layout, set, is_dynamic) + ubo_idx +
          array_index;
}

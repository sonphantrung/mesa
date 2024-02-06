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
   if (is_dynamic)
      return layout->num_ubos + layout->sets[set].dyn_ubo_offset;

   return layout->sets[set].ubo_offset;
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

unsigned
panvk_pipeline_layout_dyn_ubos_offset(const struct panvk_pipeline_layout *layout)
{
   return layout->num_ubos;
}

unsigned
panvk_pipeline_layout_dyn_desc_ubo_index(
   const struct panvk_pipeline_layout *layout)
{
   return layout->num_ubos + layout->num_dyn_ubos;
}

unsigned
panvk_pipeline_layout_total_ubo_count(
   const struct panvk_pipeline_layout *layout)
{
   return layout->num_ubos + layout->num_dyn_ubos +
          (layout->num_dyn_ssbos ? 1 : 0);
}

/*
 * Copyright © 2024 Collabora Ltd.
 *
 * Derived from tu_cmd_buffer.c which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "genxml/gen_macros.h"

#include "panvk_buffer.h"
#include "panvk_cmd_buffer.h"
#include "panvk_cmd_desc_state.h"
#include "panvk_entrypoints.h"
#include "panvk_pipeline_layout.h"

#include "util/rounding.h"

#include "vk_command_pool.h"
#include "vk_descriptor_update_template.h"

static void
panvk_emit_dyn_ubo(struct panvk_descriptor_state *desc_state,
                   const struct panvk_descriptor_set *desc_set,
                   unsigned binding, unsigned array_idx, uint32_t dyn_offset,
                   unsigned dyn_ubo_slot)
{
   struct mali_uniform_buffer_packed *ubo = &desc_state->dyn.ubos[dyn_ubo_slot];
   const struct panvk_descriptor_set_layout *slayout = desc_set->layout;
   VkDescriptorType type = slayout->bindings[binding].type;

   assert(type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
   assert(dyn_ubo_slot < ARRAY_SIZE(desc_state->dyn.ubos));

   const unsigned dyn_ubo_idx = slayout->bindings[binding].dyn_ubo_idx;
   const struct panvk_buffer_desc *bdesc =
      &desc_set->dyn_ubos[dyn_ubo_idx + array_idx];
   mali_ptr address =
      panvk_buffer_gpu_ptr(bdesc->buffer, bdesc->offset + dyn_offset);
   size_t size = panvk_buffer_range(bdesc->buffer, bdesc->offset + dyn_offset,
                                    bdesc->size);

   if (size) {
      pan_pack(ubo, UNIFORM_BUFFER, cfg) {
         cfg.pointer = address;
         cfg.entries = DIV_ROUND_UP(size, 16);
      }
   } else {
      memset(ubo, 0, sizeof(*ubo));
   }
}

static void
panvk_emit_dyn_ssbo(struct panvk_descriptor_state *desc_state,
                    const struct panvk_descriptor_set *desc_set,
                    unsigned binding, unsigned array_idx, uint32_t dyn_offset,
                    unsigned dyn_ssbo_slot)
{
   struct panvk_ssbo_addr *ssbo = &desc_state->dyn.ssbos[dyn_ssbo_slot];
   const struct panvk_descriptor_set_layout *slayout = desc_set->layout;
   VkDescriptorType type = slayout->bindings[binding].type;

   assert(type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
   assert(dyn_ssbo_slot < ARRAY_SIZE(desc_state->dyn.ssbos));

   const unsigned dyn_ssbo_idx = slayout->bindings[binding].dyn_ssbo_idx;
   const struct panvk_buffer_desc *bdesc =
      &desc_set->dyn_ssbos[dyn_ssbo_idx + array_idx];

   *ssbo = (struct panvk_ssbo_addr){
      .base_addr =
         panvk_buffer_gpu_ptr(bdesc->buffer, bdesc->offset + dyn_offset),
      .size = panvk_buffer_range(bdesc->buffer, bdesc->offset + dyn_offset,
                                 bdesc->size),
   };
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdBindDescriptorSets)(
   VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
   VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount,
   const VkDescriptorSet *pDescriptorSets, uint32_t dynamicOffsetCount,
   const uint32_t *pDynamicOffsets)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_pipeline_layout, playout, layout);

   struct panvk_descriptor_state *descriptors_state =
      panvk_cmd_get_desc_state(cmdbuf, pipelineBindPoint);

   unsigned dynoffset_idx = 0;
   for (unsigned i = 0; i < descriptorSetCount; ++i) {
      unsigned idx = i + firstSet;
      VK_FROM_HANDLE(panvk_descriptor_set, set, pDescriptorSets[i]);

      descriptors_state->sets[idx] = set;

      if (set->layout->num_dyn_ssbos || set->layout->num_dyn_ubos) {
         unsigned dyn_ubo_slot = playout->sets[idx].dyn_ubo_offset;
         unsigned dyn_ssbo_slot = playout->sets[idx].dyn_ssbo_offset;

         for (unsigned b = 0; b < set->layout->binding_count; b++) {
            for (unsigned e = 0; e < set->layout->bindings[b].array_size; e++) {
               VkDescriptorType type = set->layout->bindings[b].type;

               if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
                  panvk_emit_dyn_ubo(descriptors_state, set, b, e,
                                     pDynamicOffsets[dynoffset_idx++],
                                     dyn_ubo_slot++);
               } else if (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
                  panvk_emit_dyn_ssbo(descriptors_state, set, b, e,
                                      pDynamicOffsets[dynoffset_idx++],
                                      dyn_ssbo_slot++);
               }
            }
         }
      }
   }

   /* Unconditionally reset all previously emitted descriptors tables.
    * TODO: we could be smarter by checking which part of the pipeline layout
    * are compatible with the previouly bound descriptor sets.
    */
   descriptors_state->ubos = 0;
   descriptors_state->textures = 0;
   descriptors_state->samplers = 0;
   descriptors_state->dyn_desc_ubo = 0;
   descriptors_state->img.attrib_bufs = 0;
   descriptors_state->img.attribs = 0;

   assert(dynoffset_idx == dynamicOffsetCount);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdPushConstants)(VkCommandBuffer commandBuffer,
                                 VkPipelineLayout layout,
                                 VkShaderStageFlags stageFlags, uint32_t offset,
                                 uint32_t size, const void *pValues)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   memcpy(cmdbuf->push_constants + offset, pValues, size);

   if (stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS) {
      struct panvk_descriptor_state *desc_state = &cmdbuf->state.gfx.desc_state;

      desc_state->push_uniforms = 0;
   }

   if (stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) {
      struct panvk_descriptor_state *desc_state =
         &cmdbuf->state.compute.desc_state;

      desc_state->push_uniforms = 0;
   }
}

static struct panvk_push_descriptor_set *
panvk_cmd_push_descriptors(struct panvk_cmd_buffer *cmdbuf,
                           VkPipelineBindPoint bind_point, uint32_t set)
{
   struct panvk_descriptor_state *desc_state =
      panvk_cmd_get_desc_state(cmdbuf, bind_point);

   assert(set < MAX_SETS);
   if (unlikely(desc_state->push_sets[set] == NULL)) {
      desc_state->push_sets[set] =
         vk_zalloc(&cmdbuf->vk.pool->alloc, sizeof(*desc_state->push_sets[0]),
                   8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (unlikely(desc_state->push_sets[set] == NULL)) {
         vk_command_buffer_set_error(&cmdbuf->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return NULL;
      }
   }

   /* Pushing descriptors replaces whatever sets are bound */
   desc_state->sets[set] = NULL;

   /* Reset all descs to force emission of new tables on the next draw/dispatch.
    * TODO: Be smarter and only reset those when required.
    */
   desc_state->ubos = 0;
   desc_state->textures = 0;
   desc_state->samplers = 0;
   desc_state->img.attrib_bufs = 0;
   desc_state->img.attribs = 0;
   return desc_state->push_sets[set];
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdPushDescriptorSetKHR)(
   VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
   VkPipelineLayout layout, uint32_t set, uint32_t descriptorWriteCount,
   const VkWriteDescriptorSet *pDescriptorWrites)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_pipeline_layout, playout, layout);
   const struct panvk_descriptor_set_layout *set_layout =
      vk_to_panvk_descriptor_set_layout(playout->vk.set_layouts[set]);
   struct panvk_push_descriptor_set *push_set =
      panvk_cmd_push_descriptors(cmdbuf, pipelineBindPoint, set);
   if (!push_set)
      return;

   panvk_per_arch(push_descriptor_set)(push_set, set_layout,
                                       descriptorWriteCount, pDescriptorWrites);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdPushDescriptorSetWithTemplateKHR)(
   VkCommandBuffer commandBuffer,
   VkDescriptorUpdateTemplate descriptorUpdateTemplate, VkPipelineLayout layout,
   uint32_t set, const void *pData)
{
   VK_FROM_HANDLE(vk_descriptor_update_template, template,
                  descriptorUpdateTemplate);
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_pipeline_layout, playout, layout);
   const struct panvk_descriptor_set_layout *set_layout =
      vk_to_panvk_descriptor_set_layout(playout->vk.set_layouts[set]);
   struct panvk_push_descriptor_set *push_set =
      panvk_cmd_push_descriptors(cmdbuf, template->bind_point, set);
   if (!push_set)
      return;

   panvk_per_arch(push_descriptor_set_with_template)(
      push_set, set_layout, descriptorUpdateTemplate, pData);
}

void
panvk_per_arch(cmd_desc_state_reset)(struct panvk_cmd_buffer *cmdbuf)
{
   memset(&cmdbuf->state.gfx.desc_state.sets, 0,
          sizeof(cmdbuf->state.gfx.desc_state.sets));
   memset(&cmdbuf->state.compute.desc_state.sets, 0,
          sizeof(cmdbuf->state.compute.desc_state.sets));
}

void
panvk_per_arch(cmd_desc_state_cleanup)(struct panvk_cmd_buffer *cmdbuf)
{
   for (unsigned i = 0; i < MAX_SETS; i++) {
      if (cmdbuf->state.gfx.desc_state.push_sets[i])
         vk_free(&cmdbuf->vk.pool->alloc,
                 cmdbuf->state.gfx.desc_state.push_sets[i]);
      if (cmdbuf->state.compute.desc_state.push_sets[i])
         vk_free(&cmdbuf->vk.pool->alloc,
                 cmdbuf->state.compute.desc_state.push_sets[i]);
   }
}

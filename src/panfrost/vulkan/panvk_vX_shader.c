/*
 * Copyright © 2021 Collabora Ltd.
 *
 * Derived from tu_shader.c which is:
 * Copyright © 2019 Google LLC
 *
 * Also derived from anv_pipeline.c which is
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "genxml/gen_macros.h"

#include "panvk_device.h"
#include "panvk_instance.h"
#include "panvk_physical_device.h"
#include "panvk_pipeline.h"
#include "panvk_pipeline_layout.h"
#include "panvk_sampler.h"
#include "panvk_set_collection_layout.h"
#include "panvk_shader.h"

#include "spirv/nir_spirv.h"
#include "util/mesa-sha1.h"
#include "util/u_dynarray.h"
#include "nir_builder.h"
#include "nir_conversion_builder.h"
#include "nir_deref.h"
#include "vk_shader_module.h"

#include "compiler/bifrost_nir.h"
#include "util/pan_lower_framebuffer.h"
#include "pan_shader.h"

#include "vk_log.h"
#include "vk_util.h"

static nir_def *
load_sysval_from_push_const(nir_builder *b, nir_intrinsic_instr *intr,
                            unsigned offset)
{
   return nir_load_push_constant(
      b, intr->def.num_components, intr->def.bit_size, nir_imm_int(b, 0),
      /* Push constants are placed first, and then come the sysvals. */
      .base = offset + 256,
      .range = intr->def.num_components * intr->def.bit_size / 8);
}

static bool
panvk_lower_sysvals(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   nir_def *val = NULL;
   b->cursor = nir_before_instr(instr);

#define SYSVAL(ptype, name) offsetof(struct panvk_##ptype##_sysvals, name)
   switch (intr->intrinsic) {
   case nir_intrinsic_load_num_workgroups:
      val =
         load_sysval_from_push_const(b, intr, SYSVAL(compute, num_work_groups));
      break;
   case nir_intrinsic_load_workgroup_size:
      val = load_sysval_from_push_const(b, intr,
                                        SYSVAL(compute, local_group_size));
      break;
   case nir_intrinsic_load_viewport_scale:
      val =
         load_sysval_from_push_const(b, intr, SYSVAL(graphics, viewport.scale));
      break;
   case nir_intrinsic_load_viewport_offset:
      val = load_sysval_from_push_const(b, intr,
                                        SYSVAL(graphics, viewport.offset));
      break;
   case nir_intrinsic_load_first_vertex:
      val = load_sysval_from_push_const(b, intr,
                                        SYSVAL(graphics, vs.first_vertex));
      break;
   case nir_intrinsic_load_base_vertex:
      val =
         load_sysval_from_push_const(b, intr, SYSVAL(graphics, vs.base_vertex));
      break;
   case nir_intrinsic_load_base_instance:
      val = load_sysval_from_push_const(b, intr,
                                        SYSVAL(graphics, vs.base_instance));
      break;
   case nir_intrinsic_load_blend_const_color_rgba:
      val = load_sysval_from_push_const(b, intr,
                                        SYSVAL(graphics, blend.constants));
      break;

   case nir_intrinsic_load_layer_id:
      /* We don't support layered rendering yet, so force the layer_id to
       * zero for now.
       */
      val = nir_imm_int(b, 0);
      break;

   default:
      return false;
   }
#undef SYSVAL

   b->cursor = nir_after_instr(instr);
   nir_def_rewrite_uses(&intr->def, val);
   return true;
}

static void
shared_type_info(const struct glsl_type *type, unsigned *size, unsigned *align)
{
   assert(glsl_type_is_vector_or_scalar(type));

   uint32_t comp_size =
      glsl_type_is_boolean(type) ? 4 : glsl_get_bit_size(type) / 8;
   unsigned length = glsl_get_vector_elements(type);
   *size = comp_size * length, *align = comp_size * (length == 3 ? 4 : length);
}

struct panvk_shader *
panvk_per_arch(shader_create)(
   struct panvk_device *dev, const VkPipelineShaderStageCreateInfo *stage_info,
   struct vk_descriptor_set_layout *const *set_layouts,
   const struct panvk_set_collection_layout *layout,
   const VkAllocationCallbacks *alloc)
{
   VK_FROM_HANDLE(vk_shader_module, module, stage_info->module);
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(dev->vk.physical);
   struct panvk_instance *instance =
      to_panvk_instance(dev->vk.physical->instance);
   gl_shader_stage stage = vk_to_mesa_shader_stage(stage_info->stage);
   struct panvk_shader *shader;

   shader = vk_zalloc2(&dev->vk.alloc, alloc, sizeof(*shader), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!shader)
      return NULL;

   shader->set_layout = *layout;

   /* TODO these are made-up */
   const struct spirv_to_nir_options spirv_options = {
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = dev->vk.enabled_features.robustBufferAccess
                             ? nir_address_format_64bit_bounded_global
                             : nir_address_format_64bit_global_32bit_offset,
   };

   nir_shader *nir;
   VkResult result = vk_shader_module_to_nir(
      &dev->vk, module, stage, stage_info->pName,
      stage_info->pSpecializationInfo, &spirv_options,
      GENX(pan_shader_get_compiler_options)(), NULL, &nir);
   if (result != VK_SUCCESS) {
      vk_free2(&dev->vk.alloc, alloc, shader);
      return NULL;
   }

   NIR_PASS_V(nir, nir_lower_io_to_temporaries, nir_shader_get_entrypoint(nir),
              true, true);

   struct panfrost_compile_inputs inputs = {
      .gpu_id = phys_dev->kmod.props.gpu_prod_id,
      .no_ubo_to_push = true,
      .no_idvs = true, /* TODO */
   };

   NIR_PASS_V(nir, nir_lower_indirect_derefs,
              nir_var_shader_in | nir_var_shader_out, UINT32_MAX);

   NIR_PASS_V(nir, nir_opt_copy_prop_vars);
   NIR_PASS_V(nir, nir_opt_combine_stores, nir_var_all);
   NIR_PASS_V(nir, nir_opt_loop);

   if (stage == MESA_SHADER_FRAGMENT) {
      struct nir_input_attachment_options lower_input_attach_opts = {
         .use_fragcoord_sysval = true,
         .use_layer_id_sysval = true,
      };

      NIR_PASS_V(nir, nir_lower_input_attachments, &lower_input_attach_opts);
   }

   /* Do texture lowering here.  Yes, it's a duplication of the texture
    * lowering in bifrost_compile.  However, we need to lower texture stuff
    * now, before we call panvk_per_arch(nir_lower_descriptors)() because some
    * of the texture lowering generates nir_texop_txs which we handle as part
    * of descriptor lowering.
    *
    * TODO: We really should be doing this in common code, not dpulicated in
    * panvk.  In order to do that, we need to rework the panfrost compile
    * flow to look more like the Intel flow:
    *
    *  1. Compile SPIR-V to NIR and maybe do a tiny bit of lowering that needs
    *     to be done really early.
    *
    *  2. bi_preprocess_nir: Does common lowering and runs the optimization
    *     loop.  Nothing here should be API-specific.
    *
    *  3. Do additional lowering in panvk
    *
    *  4. bi_postprocess_nir: Does final lowering and runs the optimization
    *     loop again.  This can happen as part of the final compile.
    *
    * This would give us a better place to do panvk-specific lowering.
    */
   nir_lower_tex_options lower_tex_options = {
      .lower_txs_lod = true,
      .lower_txp = ~0,
      .lower_tg4_broadcom_swizzle = true,
      .lower_txd = true,
      .lower_invalid_implicit_lod = true,
   };
   NIR_PASS_V(nir, nir_lower_tex, &lower_tex_options);

   struct panvk_lower_desc_inputs lower_inputs = {
      .dev = dev,
      .compile_inputs = &inputs,
      .set_layouts = set_layouts,
      .layout = layout,
   };

   NIR_PASS_V(nir, panvk_per_arch(nir_lower_descriptors), &lower_inputs,
              &shader->has_img_access);

   NIR_PASS_V(nir, nir_lower_explicit_io, nir_var_mem_ubo,
              nir_address_format_32bit_index_offset);
   NIR_PASS_V(nir, nir_lower_explicit_io, nir_var_mem_ssbo,
              spirv_options.ssbo_addr_format);
   NIR_PASS_V(nir, nir_lower_explicit_io, nir_var_mem_push_const,
              nir_address_format_32bit_offset);

   if (gl_shader_stage_uses_workgroup(stage)) {
      if (!nir->info.shared_memory_explicit_layout) {
         NIR_PASS_V(nir, nir_lower_vars_to_explicit_types, nir_var_mem_shared,
                    shared_type_info);
      }

      NIR_PASS_V(nir, nir_lower_explicit_io, nir_var_mem_shared,
                 nir_address_format_32bit_offset);
   }

   NIR_PASS_V(nir, nir_lower_system_values);

   nir_lower_compute_system_values_options options = {
      .has_base_workgroup_id = false,
   };

   NIR_PASS_V(nir, nir_lower_compute_system_values, &options);

   NIR_PASS_V(nir, nir_split_var_copies);
   NIR_PASS_V(nir, nir_lower_var_copies);

   if (stage == MESA_SHADER_VERTEX) {
      /* We need the driver_location to match the vertex attribute location,
       * so we can use the attribute layout described by
       * vk_vertex_input_state where there are holes in the attribute locations.
       */
      nir_foreach_shader_in_variable(var, nir) {
         assert(var->data.location >= VERT_ATTRIB_GENERIC0 &&
                var->data.location <= VERT_ATTRIB_GENERIC15);
         var->data.driver_location = var->data.location - VERT_ATTRIB_GENERIC0;
      }
   } else {
      nir_assign_io_var_locations(nir, nir_var_shader_in, &nir->num_inputs,
                                  stage);
   }

   nir_assign_io_var_locations(nir, nir_var_shader_out, &nir->num_outputs,
                               stage);

   /* Needed to turn shader_temp into function_temp since the backend only
    * handles the latter for now.
    */
   NIR_PASS_V(nir, nir_lower_global_vars_to_local);

   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   if (unlikely(instance->debug_flags & PANVK_DEBUG_NIR)) {
      fprintf(stderr, "translated nir:\n");
      nir_print_shader(nir, stderr);
   }

   pan_shader_preprocess(nir, inputs.gpu_id);

   if (stage == MESA_SHADER_VERTEX)
      NIR_PASS_V(nir, pan_lower_image_index, MAX_VS_ATTRIBS);

   NIR_PASS_V(nir, nir_shader_instructions_pass, panvk_lower_sysvals,
              nir_metadata_block_index | nir_metadata_dominance, NULL);

   struct util_dynarray binary;
   util_dynarray_init(&binary, NULL);

   GENX(pan_shader_compile)(nir, &inputs, &binary, &shader->info);

   void *bin_ptr = util_dynarray_element(&binary, uint8_t, 0);
   unsigned bin_size = util_dynarray_num_elements(&binary, uint8_t);

   shader->bin_size = 0;
   shader->bin_ptr = NULL;

   if (bin_size) {
      void *data = malloc(bin_size);

      if (data == NULL) {
         panvk_per_arch(shader_destroy)(dev, shader, alloc);
         return NULL;
      }

      memcpy(data, bin_ptr, bin_size);
      shader->bin_size = bin_size;
      shader->bin_ptr = data;
   }
   util_dynarray_fini(&binary);

   /* Patch the descriptor count */
   shader->info.ubo_count =
      panvk_per_arch(set_collection_layout_total_ubo_count)(layout);
   shader->info.sampler_count = layout->num_samplers;
   shader->info.texture_count = layout->num_textures;

   if (stage == MESA_SHADER_VERTEX) {
      /* We leave holes in the attribute locations, but pan_shader.c assumes the
       * opposite. Patch attribute_count accordingly, so
       * pan_shader_prepare_rsd() does what we expect.
       */
      uint32_t gen_attribs =
         (shader->info.attributes_read & VERT_BIT_GENERIC_ALL) >>
         VERT_ATTRIB_GENERIC0;

      shader->info.attribute_count = util_last_bit(gen_attribs);
   }

   /* Image attributes start at MAX_VS_ATTRIBS in the VS attribute table,
    * and zero in other stages.
    */
   if (shader->has_img_access)
      shader->info.attribute_count =
         layout->num_imgs + (stage == MESA_SHADER_VERTEX ? MAX_VS_ATTRIBS : 0);

   shader->local_size.x = nir->info.workgroup_size[0];
   shader->local_size.y = nir->info.workgroup_size[1];
   shader->local_size.z = nir->info.workgroup_size[2];

   ralloc_free(nir);

   return shader;
}

void
panvk_per_arch(shader_destroy)(struct panvk_device *dev,
                               struct panvk_shader *shader,
                               const VkAllocationCallbacks *alloc)
{
   free((void *)shader->bin_ptr);
   vk_free2(&dev->vk.alloc, alloc, shader);
}

void
panvk_per_arch(set_collection_layout_fill)(
   struct panvk_set_collection_layout *layout, unsigned set_layout_count,
   struct vk_descriptor_set_layout *const *set_layouts)
{
   layout->set_count = set_layout_count;

   unsigned sampler_idx = 0, tex_idx = 0, ubo_idx = 0;
   unsigned dyn_ubo_idx = 0, dyn_ssbo_idx = 0, img_idx = 0;
   unsigned dyn_desc_ubo_offset = 0;

   for (unsigned set = 0; set < layout->set_count; set++) {
      /* TODO: Handle missing set layout */
      if (set_layouts[set] == NULL)
         continue;

      const struct panvk_descriptor_set_layout *set_layout =
         vk_to_panvk_descriptor_set_layout(set_layouts[set]);

      layout->sets[set].sampler_offset = sampler_idx;
      layout->sets[set].tex_offset = tex_idx;
      layout->sets[set].ubo_offset = ubo_idx;
      layout->sets[set].dyn_ubo_offset = dyn_ubo_idx;
      layout->sets[set].dyn_ssbo_offset = dyn_ssbo_idx;
      layout->sets[set].img_offset = img_idx;
      layout->sets[set].dyn_desc_ubo_offset = dyn_desc_ubo_offset;

      layout->sets[set].num_ubos = set_layout->num_ubos;
      layout->sets[set].num_dyn_ubos = set_layout->num_dyn_ubos;

      sampler_idx += set_layout->num_samplers;
      tex_idx += set_layout->num_textures;
      ubo_idx += set_layout->num_ubos;
      dyn_ubo_idx += set_layout->num_dyn_ubos;
      dyn_ssbo_idx += set_layout->num_dyn_ssbos;
      img_idx += set_layout->num_imgs;
      dyn_desc_ubo_offset +=
         set_layout->num_dyn_ssbos * sizeof(struct panvk_ssbo_addr);
   }

   layout->num_samplers = sampler_idx;
   layout->num_textures = tex_idx;
   layout->num_ubos = ubo_idx;
   layout->num_dyn_ubos = dyn_ubo_idx;
   layout->num_dyn_ssbos = dyn_ssbo_idx;
   layout->num_imgs = img_idx;

   /* Some NIR texture operations don't require a sampler, but Bifrost/Midgard
    * ones always expect one. Add a dummy sampler to deal with this limitation.
    */
   if (layout->num_textures) {
      layout->num_samplers++;
      for (unsigned set = 0; set < layout->set_count; set++)
         layout->sets[set].sampler_offset++;
   }
}

void
panvk_per_arch(set_collection_layout_hash_state)(
   const struct panvk_set_collection_layout *layout,
   struct vk_descriptor_set_layout *const *set_layouts,
   struct mesa_sha1 *sha1_ctx)
{
   for (unsigned set = 0; set < layout->set_count; set++) {
      const struct panvk_descriptor_set_layout *set_layout =
         vk_to_panvk_descriptor_set_layout(set_layouts[set]);

      for (unsigned b = 0; b < set_layout->binding_count; b++) {
         const struct panvk_descriptor_set_binding_layout *binding_layout =
            &set_layout->bindings[b];

         if (binding_layout->immutable_samplers) {
            for (unsigned s = 0; s < binding_layout->array_size; s++) {
               struct panvk_sampler *sampler =
                  binding_layout->immutable_samplers[s];

               _mesa_sha1_update(sha1_ctx, &sampler->desc,
                                 sizeof(sampler->desc));
            }
         }
         _mesa_sha1_update(sha1_ctx, &binding_layout->type,
                           sizeof(binding_layout->type));
         _mesa_sha1_update(sha1_ctx, &binding_layout->array_size,
                           sizeof(binding_layout->array_size));
         _mesa_sha1_update(sha1_ctx, &binding_layout->shader_stages,
                           sizeof(binding_layout->shader_stages));
      }
   }
}

unsigned
panvk_per_arch(set_collection_layout_ubo_start)(
   const struct panvk_set_collection_layout *layout, unsigned set,
   bool is_dynamic)
{
   if (is_dynamic)
      return layout->num_ubos + layout->sets[set].dyn_ubo_offset;

   return layout->sets[set].ubo_offset;
}

unsigned
panvk_per_arch(set_collection_layout_ubo_index)(
   const struct panvk_set_collection_layout *layout,
   struct vk_descriptor_set_layout *const *set_layouts, unsigned set,
   unsigned binding, unsigned array_index)
{
   const struct panvk_descriptor_set_layout *set_layout =
      vk_to_panvk_descriptor_set_layout(set_layouts[set]);
   const struct panvk_descriptor_set_binding_layout *binding_layout =
      &set_layout->bindings[binding];

   const bool is_dynamic =
      binding_layout->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
   const uint32_t ubo_idx =
      is_dynamic ? binding_layout->dyn_ubo_idx : binding_layout->ubo_idx;

   return panvk_per_arch(set_collection_layout_ubo_start)(layout, set,
                                                          is_dynamic) +
          ubo_idx + array_index;
}

unsigned
panvk_per_arch(set_collection_layout_dyn_desc_ubo_index)(
   const struct panvk_set_collection_layout *layout)
{
   return layout->num_ubos + layout->num_dyn_ubos;
}

unsigned
panvk_per_arch(set_collection_layout_dyn_ubos_offset)(
   const struct panvk_set_collection_layout *layout)
{
   return layout->num_ubos;
}

unsigned
panvk_per_arch(set_collection_layout_total_ubo_count)(
   const struct panvk_set_collection_layout *layout)
{
   return layout->num_ubos + layout->num_dyn_ubos +
          (layout->num_dyn_ssbos ? 1 : 0);
}

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

#include "panvk_cmd_buffer.h"
#include "panvk_descriptor_set_layout.h"
#include "panvk_device.h"
#include "panvk_instance.h"
#include "panvk_physical_device.h"
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
#include "vk_pipeline.h"
#include "vk_shader.h"
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

static inline nir_address_format
panvk_buffer_ubo_addr_format(VkPipelineRobustnessBufferBehaviorEXT robustness)
{
   switch (robustness) {
   case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT:
   case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT:
   case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT:
      return nir_address_format_32bit_index_offset;
   default:
      unreachable("Invalid robust buffer access behavior");
   }
}

static inline nir_address_format
panvk_buffer_ssbo_addr_format(VkPipelineRobustnessBufferBehaviorEXT robustness)
{
   switch (robustness) {
   case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT:
      return nir_address_format_64bit_global_32bit_offset;
   case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT:
   case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT:
      return nir_address_format_64bit_bounded_global;
   default:
      unreachable("Invalid robust buffer access behavior");
   }
}

static const nir_shader_compiler_options *
panvk_get_nir_options(UNUSED struct vk_physical_device *vk_pdev,
                      UNUSED gl_shader_stage stage,
                      UNUSED const struct vk_pipeline_robustness_state *rs)
{
   return GENX(pan_shader_get_compiler_options)();
}

static struct spirv_to_nir_options
panvk_get_spirv_options(UNUSED struct vk_physical_device *vk_pdev,
                        UNUSED gl_shader_stage stage,
                        const struct vk_pipeline_robustness_state *rs)
{
   return (struct spirv_to_nir_options){
      .ubo_addr_format = panvk_buffer_ubo_addr_format(rs->uniform_buffers),
      .ssbo_addr_format = panvk_buffer_ssbo_addr_format(rs->storage_buffers),
   };
}

static void
panvk_preprocess_nir(UNUSED struct vk_physical_device *vk_pdev, nir_shader *nir)
{
   NIR_PASS_V(nir, nir_lower_io_to_temporaries, nir_shader_get_entrypoint(nir),
              true, true);

   NIR_PASS_V(nir, nir_lower_indirect_derefs,
              nir_var_shader_in | nir_var_shader_out, UINT32_MAX);

   NIR_PASS_V(nir, nir_opt_copy_prop_vars);
   NIR_PASS_V(nir, nir_opt_combine_stores, nir_var_all);
   NIR_PASS_V(nir, nir_opt_loop);

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
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
    *  2. pan_preprocess_nir: Does common lowering and runs the optimization
    *     loop.  Nothing here should be API-specific.
    *
    *  3. Do additional lowering in panvk
    *
    *  4. pan_postprocess_nir: Does final lowering and runs the optimization
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
   NIR_PASS_V(nir, nir_lower_system_values);

   nir_lower_compute_system_values_options options = {
      .has_base_workgroup_id = false,
   };

   NIR_PASS_V(nir, nir_lower_compute_system_values, &options);

   NIR_PASS_V(nir, nir_split_var_copies);
   NIR_PASS_V(nir, nir_lower_var_copies);
}

static void
panvk_hash_graphics_state(struct vk_physical_device *device,
                          const struct vk_graphics_pipeline_state *state,
                          VkShaderStageFlags stages, blake3_hash blake3_out)
{
   struct mesa_blake3 blake3_ctx;
   _mesa_blake3_init(&blake3_ctx);

   /* We don't need to do anything here yet */

   _mesa_blake3_final(&blake3_ctx, blake3_out);
}

static void
panvk_lower_nir(struct panvk_device *dev, nir_shader *nir,
                struct vk_descriptor_set_layout *const *set_layouts,
                const struct vk_pipeline_robustness_state *rs,
                const struct panvk_set_collection_layout *layout,
                const struct panfrost_compile_inputs *compile_input,
                bool *has_img_access)
{
   struct panvk_instance *instance =
      to_panvk_instance(dev->vk.physical->instance);
   gl_shader_stage stage = nir->info.stage;

   /* TODO */
   struct panvk_lower_desc_inputs lower_inputs = {
      .dev = dev,
      .compile_inputs = compile_input,
      .layout = layout,
      .set_layouts = set_layouts,
   };

   NIR_PASS_V(nir, panvk_per_arch(nir_lower_descriptors), &lower_inputs,
              has_img_access);

   NIR_PASS_V(nir, nir_lower_explicit_io, nir_var_mem_ubo,
              panvk_buffer_ubo_addr_format(rs->uniform_buffers));
   NIR_PASS_V(nir, nir_lower_explicit_io, nir_var_mem_ssbo,
              panvk_buffer_ssbo_addr_format(rs->storage_buffers));
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

   pan_shader_preprocess(nir, compile_input->gpu_id);

   if (stage == MESA_SHADER_VERTEX)
      NIR_PASS_V(nir, pan_lower_image_index, MAX_VS_ATTRIBS);

   NIR_PASS_V(nir, nir_shader_instructions_pass, panvk_lower_sysvals,
              nir_metadata_block_index | nir_metadata_dominance, NULL);
}

static VkResult
panvk_compile_nir(struct panvk_device *dev, nir_shader *nir,
                  VkShaderCreateFlagsEXT shader_flags,
                  struct panfrost_compile_inputs *compile_input,
                  struct panvk_shader *shader)
{
   const bool dump_asm =
      shader_flags & VK_SHADER_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_MESA;

   /* TODO: ASM dumping (VK_KHR_maintenance5) */
   assert(!dump_asm);

   struct util_dynarray binary;
   util_dynarray_init(&binary, NULL);
   GENX(pan_shader_compile)(nir, compile_input, &binary, &shader->info);

   void *bin_ptr = util_dynarray_element(&binary, uint8_t, 0);
   unsigned bin_size = util_dynarray_num_elements(&binary, uint8_t);

   shader->bin_size = 0;
   shader->bin_ptr = NULL;

   if (bin_size) {
      void *data = malloc(bin_size);

      if (data == NULL)
         return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

      memcpy(data, bin_ptr, bin_size);
      shader->bin_size = bin_size;
      shader->bin_ptr = data;
   }
   util_dynarray_fini(&binary);

   /* Patch the descriptor count */
   shader->info.ubo_count = panvk_per_arch(
      set_collection_layout_total_ubo_count)(&shader->set_layout);
   shader->info.sampler_count = shader->set_layout.num_samplers;
   shader->info.texture_count = shader->set_layout.num_textures;

   if (nir->info.stage == MESA_SHADER_VERTEX) {
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
         shader->set_layout.num_imgs +
         (nir->info.stage == MESA_SHADER_VERTEX ? MAX_VS_ATTRIBS : 0);

   shader->local_size.x = nir->info.workgroup_size[0];
   shader->local_size.y = nir->info.workgroup_size[1];
   shader->local_size.z = nir->info.workgroup_size[2];

   return VK_SUCCESS;
}

static VkResult
panvk_shader_upload(struct panvk_device *dev, struct panvk_shader *shader,
                    const VkAllocationCallbacks *pAllocator)
{
   uint32_t code_sz = ALIGN(shader->bin_size, 128);

   if (code_sz > 0) {
      /* TODO: Upload shader in a Vulkan device wide shader pool */
      shader->upload_bo =
         panvk_priv_bo_create(dev, code_sz, PAN_KMOD_BO_FLAG_EXECUTABLE,
                              pAllocator, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

      if (shader->upload_bo == NULL)
         return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);

      memcpy(shader->upload_bo->addr.host, shader->bin_ptr, shader->bin_size);
      shader->upload_addr = shader->upload_bo->addr.dev;
      shader->upload_size = code_sz;
   }

   shader->upload_size = code_sz;

   return VK_SUCCESS;
}

static void
panvk_shader_destroy(struct vk_device *vk_dev, struct vk_shader *vk_shader,
                     const VkAllocationCallbacks *pAllocator)
{
   struct panvk_device *dev = to_panvk_device(vk_dev);
   struct panvk_shader *shader =
      container_of(vk_shader, struct panvk_shader, vk);

   panvk_priv_bo_destroy(shader->upload_bo, pAllocator);

   free((void *)shader->bin_ptr);
   vk_shader_free(&dev->vk, pAllocator, &shader->vk);
}

static const struct vk_shader_ops panvk_shader_ops;

static VkResult
panvk_compile_shader(struct panvk_device *dev,
                     struct vk_shader_compile_info *info,
                     const struct vk_graphics_pipeline_state *state,
                     const VkAllocationCallbacks *pAllocator,
                     struct vk_shader **shader_out)
{
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(dev->vk.physical);

   struct panvk_shader *shader;
   VkResult result;

   /* We consume the NIR, regardless of success or failure */
   nir_shader *nir = info->nir;

   shader = vk_shader_zalloc(&dev->vk, &panvk_shader_ops, info->stage,
                             pAllocator, sizeof(*shader));
   if (shader == NULL)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   panvk_per_arch(set_collection_layout_fill)(
      &shader->set_layout, info->set_layout_count, info->set_layouts);

   struct panfrost_compile_inputs inputs = {
      .gpu_id = phys_dev->kmod.props.gpu_prod_id,
      .no_ubo_to_push = true,
      .no_idvs = true, /* TODO */
   };

   panvk_lower_nir(dev, nir, info->set_layouts, info->robustness,
                   &shader->set_layout, &inputs, &shader->has_img_access);

   result = panvk_compile_nir(dev, nir, info->flags, &inputs, shader);

   if (result != VK_SUCCESS) {
      panvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);
      return result;
   }

   result = panvk_shader_upload(dev, shader, pAllocator);

   if (result != VK_SUCCESS) {
      panvk_shader_destroy(&dev->vk, &shader->vk, pAllocator);
      return result;
   }

   *shader_out = &shader->vk;

   return result;
}

static VkResult
panvk_compile_shaders(struct vk_device *vk_dev, uint32_t shader_count,
                      struct vk_shader_compile_info *infos,
                      const struct vk_graphics_pipeline_state *state,
                      const VkAllocationCallbacks *pAllocator,
                      struct vk_shader **shaders_out)
{
   struct panvk_device *dev = to_panvk_device(vk_dev);
   VkResult result;
   uint32_t i;

   for (i = 0; i < shader_count; i++) {
      result = panvk_compile_shader(dev, &infos[i], state, pAllocator,
                                    &shaders_out[i]);

      /* Clean up NIR for the current shader */
      ralloc_free(infos[i].nir);

      if (result != VK_SUCCESS)
         goto err_cleanup;
   }

   /* TODO: If we get multiple shaders here, we can perform part of the link
    * logic at compile time. */

   return VK_SUCCESS;

err_cleanup:
   /* Clean up all the shaders before this point */
   for (uint32_t j = 0; j < i; j++)
      panvk_shader_destroy(&dev->vk, shaders_out[j], pAllocator);

   /* Clean up all the NIR after this point */
   for (uint32_t j = i + 1; j < shader_count; j++)
      ralloc_free(infos[j].nir);

   /* Memset the output array */
   memset(shaders_out, 0, shader_count * sizeof(*shaders_out));

   return result;
}

static VkResult
panvk_deserialize_shader(struct vk_device *vk_dev, struct blob_reader *blob,
                         uint32_t binary_version,
                         const VkAllocationCallbacks *pAllocator,
                         struct vk_shader **shader_out)
{
   struct panvk_device *device = to_panvk_device(vk_dev);
   struct panvk_shader *shader;
   VkResult result;

   struct pan_shader_info info;
   blob_copy_bytes(blob, &info, sizeof(info));

   struct panvk_set_collection_layout set_layout;
   blob_copy_bytes(blob, &set_layout, sizeof(set_layout));

   struct pan_compute_dim local_size;
   blob_copy_bytes(blob, &local_size, sizeof(local_size));

   const bool has_img_access = blob_read_uint32(blob);
   const uint32_t bin_size = blob_read_uint32(blob);

   if (blob->overrun)
      return vk_error(device, VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT);

   shader = vk_shader_zalloc(vk_dev, &panvk_shader_ops, info.stage, pAllocator,
                             sizeof(*shader));
   if (shader == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   shader->info = info;
   shader->set_layout = set_layout;
   shader->local_size = local_size;
   shader->has_img_access = has_img_access;
   shader->bin_size = bin_size;

   shader->bin_ptr = malloc(bin_size);
   if (shader->bin_ptr == NULL) {
      panvk_shader_destroy(vk_dev, &shader->vk, pAllocator);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   blob_copy_bytes(blob, (void *)shader->bin_ptr, shader->bin_size);

   if (blob->overrun) {
      panvk_shader_destroy(vk_dev, &shader->vk, pAllocator);
      return vk_error(device, VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT);
   }

   result = panvk_shader_upload(device, shader, pAllocator);

   if (result != VK_SUCCESS) {
      panvk_shader_destroy(vk_dev, &shader->vk, pAllocator);
      return result;
   }

   *shader_out = &shader->vk;

   return result;
}

static bool
panvk_shader_serialize(struct vk_device *vk_dev,
                       const struct vk_shader *vk_shader, struct blob *blob)
{
   struct panvk_shader *shader =
      container_of(vk_shader, struct panvk_shader, vk);

   /* TODO: Disallow serialization with assembly when implemented */
   /* TODO: Implement serialization with assembly */

   blob_write_bytes(blob, &shader->info, sizeof(shader->info));
   blob_write_bytes(blob, &shader->set_layout, sizeof(shader->set_layout));
   blob_write_bytes(blob, &shader->local_size, sizeof(shader->local_size));
   blob_write_uint32(blob, shader->has_img_access);
   blob_write_uint32(blob, shader->bin_size);
   blob_write_bytes(blob, shader->bin_ptr, shader->bin_size);

   return !blob->out_of_memory;
}

#define WRITE_STR(field, ...)                                                  \
   ({                                                                          \
      memset(field, 0, sizeof(field));                                         \
      UNUSED int i = snprintf(field, sizeof(field), __VA_ARGS__);              \
      assert(i > 0 && i < sizeof(field));                                      \
   })

static VkResult
panvk_shader_get_executable_properties(
   UNUSED struct vk_device *device, const struct vk_shader *vk_shader,
   uint32_t *executable_count, VkPipelineExecutablePropertiesKHR *properties)
{
   UNUSED struct panvk_shader *shader =
      container_of(vk_shader, struct panvk_shader, vk);

   VK_OUTARRAY_MAKE_TYPED(VkPipelineExecutablePropertiesKHR, out, properties,
                          executable_count);

   vk_outarray_append_typed(VkPipelineExecutablePropertiesKHR, &out, props)
   {
      props->stages = mesa_to_vk_shader_stage(shader->info.stage);
      props->subgroupSize = 8;
      WRITE_STR(props->name, "%s",
                _mesa_shader_stage_to_string(shader->info.stage));
      WRITE_STR(props->description, "%s shader",
                _mesa_shader_stage_to_string(shader->info.stage));
   }

   return vk_outarray_status(&out);
}

static VkResult
panvk_shader_get_executable_statistics(
   UNUSED struct vk_device *device, const struct vk_shader *vk_shader,
   uint32_t executable_index, uint32_t *statistic_count,
   VkPipelineExecutableStatisticKHR *statistics)
{
   UNUSED struct panvk_shader *shader =
      container_of(vk_shader, struct panvk_shader, vk);

   VK_OUTARRAY_MAKE_TYPED(VkPipelineExecutableStatisticKHR, out, statistics,
                          statistic_count);

   assert(executable_index == 0);

   vk_outarray_append_typed(VkPipelineExecutableStatisticKHR, &out, stat)
   {
      WRITE_STR(stat->name, "Code Size");
      WRITE_STR(stat->description,
                "Size of the compiled shader binary, in bytes");
      stat->format = VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;
      stat->value.u64 = shader->bin_size;
   }

   /* TODO: more executable statistics (VK_KHR_pipeline_executable_properties) */

   return vk_outarray_status(&out);
}

static VkResult
panvk_shader_get_executable_internal_representations(
   UNUSED struct vk_device *device, const struct vk_shader *vk_shader,
   uint32_t executable_index, uint32_t *internal_representation_count,
   VkPipelineExecutableInternalRepresentationKHR *internal_representations)
{
   UNUSED struct panvk_shader *shader =
      container_of(vk_shader, struct panvk_shader, vk);
   VK_OUTARRAY_MAKE_TYPED(VkPipelineExecutableInternalRepresentationKHR, out,
                          internal_representations,
                          internal_representation_count);
   bool incomplete_text = false;

   /* TODO: Compiler assembly (VK_KHR_pipeline_executable_properties) */

   return incomplete_text ? VK_INCOMPLETE : vk_outarray_status(&out);
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

static const struct vk_shader_ops panvk_shader_ops = {
   .destroy = panvk_shader_destroy,
   .serialize = panvk_shader_serialize,
   .get_executable_properties = panvk_shader_get_executable_properties,
   .get_executable_statistics = panvk_shader_get_executable_statistics,
   .get_executable_internal_representations =
      panvk_shader_get_executable_internal_representations,
};

const struct vk_device_shader_ops panvk_per_arch(device_shader_ops) = {
   .get_nir_options = panvk_get_nir_options,
   .get_spirv_options = panvk_get_spirv_options,
   .preprocess_nir = panvk_preprocess_nir,
   .hash_graphics_state = panvk_hash_graphics_state,
   .compile = panvk_compile_shaders,
   .deserialize = panvk_deserialize_shader,
   .cmd_set_dynamic_graphics_state = vk_cmd_set_dynamic_graphics_state,
   .cmd_bind_shaders = panvk_per_arch(cmd_bind_shaders),
};
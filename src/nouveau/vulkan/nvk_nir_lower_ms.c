/*
 * Copyright © 2023 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */
#include "nvk_shader.h"

#include "nir_builder.h"
#include "nir_deref.h"

static bool
lower_set_vertex_and_primitive_count(nir_builder *b,
                                     nir_intrinsic_instr *intrin,
                                     void *data)
{
   if (intrin->intrinsic != nir_intrinsic_set_vertex_and_primitive_count)
      return false;

   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *local_invocation_index = nir_load_local_invocation_index(b);
   nir_push_if(b, nir_ieq(b, local_invocation_index, nir_imm_int(b, 0)));
   {
      nir_set_vertex_and_primitive_count(b, intrin->src[0].ssa,
                                         intrin->src[1].ssa,
                                         intrin->src[2].ssa);
   }
   nir_pop_if(b, NULL);

   nir_instr_remove(&intrin->instr);

   return true;
}

static bool
lower_mesh_workgroup_id_intrin(nir_builder *b, nir_intrinsic_instr *intrin,
                               void *data)
{
   if (intrin->intrinsic != nir_intrinsic_load_local_invocation_id)
      return false;

   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *workgroup_index = nir_load_local_invocation_index(b);
   nir_def *workgroup_size = nir_imm_ivec3(b, b->shader->info.workgroup_size[0],
                                           b->shader->info.workgroup_size[1],
                                           b->shader->info.workgroup_size[2]);

   nir_def *comps[3];
   assert(intrin->def.num_components <= 3);

   nir_def *workgroup_size_x = nir_channel(b, workgroup_size, 0);
   nir_def *workgroup_size_y = nir_channel(b, workgroup_size, 1);
   nir_def *workgroup_size_z = nir_channel(b, workgroup_size, 2);

   comps[0] = nir_imod(b, workgroup_index, workgroup_size_x);
   comps[1] = nir_imod(b, nir_idiv(b, workgroup_index, workgroup_size_x), workgroup_size_y);
   comps[2] = nir_imod(b, nir_idiv(b, workgroup_index, nir_imul(b, workgroup_size_x, workgroup_size_y)), workgroup_size_z);

   nir_def *workgroup_id = nir_vec(b, comps, intrin->def.num_components);
   nir_def_rewrite_uses(&intrin->def, workgroup_id);
   nir_instr_remove(&intrin->instr);

   return true;
}

static bool
lower_local_invocation_index_to_arg(nir_builder *b, nir_instr *instr,
                                    void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_load_local_invocation_index)
      return false;

   b->cursor = nir_instr_remove(instr);
   nir_def *local_invocation_index = nir_load_param(b, 0);
   nir_def_rewrite_uses(&intr->def, local_invocation_index);
   return true;
}

bool nvk_nir_lower_mesh(nir_shader *nir) {
   /* First, we avoid ensure that set_vertex_and_primitive_count will only be called on the first local invocation */
   nir_shader_intrinsics_pass(nir, lower_set_vertex_and_primitive_count,
                              nir_metadata_none,
                              NULL);

   /* We then lower the workgroup and local invocation ids to use their respective index */
   nir_shader_intrinsics_pass(nir, lower_mesh_workgroup_id_intrin,
                              nir_metadata_block_index | nir_metadata_dominance,
                              NULL);

   /* 
    * We create a new function that will contains the old entrypoint and make it takes a single argument (the local invocation index)
    * At that point we assume that everything was inlined in the entrypoint
    */
   nir_function *ms_function = nir_function_create(nir, "ms_entrypoint");
   ms_function->impl = nir_function_impl_clone(nir, nir_shader_get_entrypoint(nir));
   ms_function->impl->function = ms_function;

   ms_function->num_params = 1;
   ms_function->params = rzalloc_array(nir, nir_parameter, 1);
   ms_function->params[0] = (nir_parameter){1, 32};

   /* We now lower load_local_invocation_index to use the function argument */
   nir_function_instructions_pass(
      ms_function->impl, lower_local_invocation_index_to_arg,
      nir_metadata_block_index | nir_metadata_dominance, NULL);

   /* We create a new entrypoint */
   nir_function *old_entry_function = nir_shader_get_entrypoint(nir)->function;
   nir_function *entry_function = nir_function_clone(nir, old_entry_function);
   entry_function->impl = nir_function_impl_create_bare(nir);
   entry_function->impl->function = entry_function;
   old_entry_function->is_entrypoint = false;

   /* We now call the previous entrypoint function with an adjusted local invocation index */
   nir_builder b = nir_builder_at(nir_before_impl(entry_function->impl));

   nir_def *real_local_invocation_index = nir_load_local_invocation_index(&b);

   const uint32_t local_size = nir->info.workgroup_size[0] *
                               nir->info.workgroup_size[1] *
                               nir->info.workgroup_size[2];

   const uint32_t local_invocation_group_count = DIV_ROUND_UP(local_size, WARP_SIZE);
   const uint32_t wrap_aligned_local_size = local_invocation_group_count * WARP_SIZE;
   assert(local_invocation_group_count > 0);

   for (uint32_t index = 0; index < local_invocation_group_count; index++) {
      nir_def *local_invocation_index = nir_iadd(&b, real_local_invocation_index, nir_imm_int(&b, index * WARP_SIZE));

      bool need_bound_checks = local_size != wrap_aligned_local_size && index == local_invocation_group_count - 1;

      if (need_bound_checks) {
         nir_push_if(&b, nir_ilt_imm(&b, local_invocation_index, local_size));
      }

      nir_call(&b, ms_function, local_invocation_index);

      if (need_bound_checks) {
         nir_pop_if(&b, NULL);
      }
   }

   /* Finally, we get ride of the old functions */
   nir_inline_functions(nir);
   exec_node_remove(&ms_function->node);
   exec_node_remove(&old_entry_function->node);

   /* And destroy the metadata as everything changed */
   nir_metadata_preserve(entry_function->impl, nir_metadata_none);

   return true;
}

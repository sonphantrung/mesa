/*
 * Copyright © 2024 Igalia
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "nir.h"
#include "nir_builder.h"
#include "nir_builder_opcodes.h"
#include "nir_intrinsics.h"

/*
 * Convert 64-bit atomic arithmetic to regular arithmetic along with cmpxchg
 * by repeating the operation until the result is expected.
 * This is the same as the blob driver does.
 *
 * eg:
 * atomicAdd(a[0], 1) ->
 *
 * uint expected = a[0];
 * while (true) {
 *    uint before = expected;
 *    expected += 1;
 *    uint original = atomicCompareExchange(a[0], before, expected);
 *    if (original == before) {break;}
 *    expected = original;
 * }
 */

static nir_def *
build_emulated_atomic(nir_builder *b, nir_intrinsic_instr *intr, bool ssbo)
{
   nir_def *load = ssbo ? nir_load_ssbo(b, 1, 64, intr->src[0].ssa,
                                        intr->src[1].ssa, .align_mul = 8)
                        : nir_load_global(b, intr->src[0].ssa, 8, 1, 64);
   nir_def *data = ssbo ? intr->src[2].ssa : intr->src[1].ssa;
   nir_loop *loop = nir_push_loop(b);
   nir_def *xchg;
   {
      nir_phi_instr *phi = nir_phi_instr_create(b->shader);
      nir_def_init(&phi->instr, &phi->def, 1, 64);
      nir_phi_instr_add_src(phi, load->parent_instr->block, load);
      nir_def *before = &phi->def;
      nir_def *expected = nir_build_alu2(
         b, nir_atomic_op_to_alu(nir_intrinsic_atomic_op(intr)), before, data);
      if (ssbo) {
         xchg = nir_ssbo_atomic_swap(b, 64, intr->src[0].ssa, intr->src[1].ssa,
                                     before, expected,
                                     .atomic_op = nir_atomic_op_cmpxchg);
      } else {
         xchg =
            nir_global_atomic_swap(b, 64, intr->src[0].ssa, before, expected,
                                   .atomic_op = nir_atomic_op_cmpxchg);
      }
      nir_push_if(b, nir_ieq(b, xchg, before));
      {
         nir_jump(b, nir_jump_break);
      }
      nir_pop_if(b, NULL);
      nir_phi_instr_add_src(phi, nir_loop_last_block(loop), xchg);
      b->cursor = nir_before_block(nir_loop_first_block(loop));
      nir_builder_instr_insert(b, &phi->instr);
   }
   nir_pop_loop(b, loop);
   nir_def *result = nir_mov(b, xchg);
   return result;
}

static bool
emulate_64b_atomics(struct nir_builder *b, nir_intrinsic_instr *intr,
                    void* supported)
{
   nir_instr_filter_cb supported_cb = supported;

   if (intr->intrinsic != nir_intrinsic_ssbo_atomic &&
       intr->intrinsic != nir_intrinsic_global_atomic)
      return false;
   if (intr->def.bit_size != 64)
      return false;
   if (supported_cb(&intr->instr, NULL))
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   switch (nir_intrinsic_atomic_op(intr)) {
   case nir_atomic_op_imin:
   case nir_atomic_op_umin:
   case nir_atomic_op_imax:
   case nir_atomic_op_umax:
   case nir_atomic_op_iand:
   case nir_atomic_op_ior:
   case nir_atomic_op_ixor:
   case nir_atomic_op_fadd:
   case nir_atomic_op_fmin:
   case nir_atomic_op_fmax:
   case nir_atomic_op_iadd: {
      nir_def_rewrite_uses(
         &intr->def, build_emulated_atomic(
                        b, intr, intr->intrinsic == nir_intrinsic_ssbo_atomic));
      nir_instr_remove(&intr->instr);
      return true;
      break;
   }
   /* Lowered in ir3. */
   case nir_atomic_op_cmpxchg:
   case nir_atomic_op_xchg:
   case nir_atomic_op_fcmpxchg:
      return false;
   default:
      unreachable("Invalid nir_atomic_op");
   }
}

/*
 * Some 64b atomics are not supported in hardware, but can be implemented in
 * other terms.
 */
bool
nir_emulate_64b_atomics(nir_shader *shader, nir_instr_filter_cb supported)
{
   return nir_shader_intrinsics_pass(shader, emulate_64b_atomics,
                                     nir_metadata_none, supported);
}

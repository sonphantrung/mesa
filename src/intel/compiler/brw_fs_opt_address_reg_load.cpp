/*
 * Copyright © 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "brw_fs.h"
#include "brw_fs_builder.h"
#include "brw_cfg.h"
#include "brw_eu.h"

/** @file brw_fs_opt_address_reg_load.cpp
 *
 * Turn this sequence :
 *
 *    add(8) vgrf64:UD, vgrf63:UD,        192u
 *    mov(1)   a0.4:UD, vgrf64+0.0<0>:UD
 *
 * into :
 *
 *    add(1)   a0.4:UD, vgrf63+0.0<0>:UD, 192u
 */

using namespace brw;

static bool
propagate_reg_load(fs_visitor &s, bblock_t *block, fs_inst *inst)
{
   foreach_inst_in_block_reverse_starting_from(fs_inst, scan_inst, inst) {
      if (scan_inst->dst.file != inst->src[0].file ||
          scan_inst->dst.nr != inst->src[0].nr)
         continue;

      switch (scan_inst->opcode) {
      case BRW_OPCODE_MOV:
      case BRW_OPCODE_ADD:
         break;

      default:
         continue;
      }

      fs_builder ubld = fs_builder(&s).at(block, inst).exec_all().group(1, 0);
      fs_reg sources[4];
      for (unsigned i = 0; i < scan_inst->sources; i++) {
         sources[i] = inst->src[i].file == VGRF ? component(scan_inst->src[i], 0) : scan_inst->src[i];
      }
      ubld.emit(scan_inst->opcode, inst->dst, sources, scan_inst->sources);

      inst->remove(block);
      return true;
   }

   return false;
}

static bool
opt_address_reg_load_local(fs_visitor &s, bblock_t *block)
{
   bool progress = false;

   foreach_inst_in_block_reverse_safe(fs_inst, inst, block) {
      if (!inst->dst.is_address())
         continue;

      switch (inst->opcode) {
      case BRW_OPCODE_MOV:
         break;

      default:
         continue;
      }

      progress = propagate_reg_load(s, block, inst) || progress;
   }

   return progress;
}

bool
brw_fs_opt_address_reg_load(fs_visitor &s)
{
   bool progress = false;

   foreach_block_reverse(block, s.cfg) {
      progress = opt_address_reg_load_local(s, block) || progress;
   }

   if (progress) {
      s.cfg->adjust_block_ips();

      s.invalidate_analysis(DEPENDENCY_INSTRUCTIONS);
   }

   return progress;
}

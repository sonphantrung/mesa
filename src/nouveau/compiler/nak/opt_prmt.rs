/*
 * Copyright © 2023 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use std::collections::HashMap;

use crate::ir::*;

struct PrmtEntry {
    srcs: [Src; 2],
    selector_value: u32,
}

struct PrmtPass {
    ssa_prmt: HashMap<SSAValue, PrmtEntry>,
}

fn extract_byte_select(selector: u32, index: u32) -> (u32, u32) {
    let byte_selected = (selector >> (index * 4)) & 0x7;
    let signed_mod = (selector >> (index * 4)) & 0x8;

    (byte_selected, signed_mod)
}

fn is_byte_using_src(selector: u32, byte_index: u32, src_idx: usize) -> bool {
    let (byte_selected, _) = extract_byte_select(selector, byte_index);

    match src_idx {
        0 if byte_selected < 4 => true,
        1 if byte_selected > 3 => true,
        _ => false,
    }
}

fn is_src_used(selector: u32, src_idx: usize) -> bool {
    for byte_index in 0..4 {
        if is_byte_using_src(selector, byte_index, src_idx) {
            return true;
        }
    }

    false
}

impl PrmtPass {
    fn new() -> PrmtPass {
        PrmtPass {
            ssa_prmt: HashMap::new(),
        }
    }

    fn add_prmt(
        &mut self,
        ssa: SSAValue,
        mode: PrmtMode,
        srcs: [Src; 2],
        sel: Src,
    ) {
        if mode != PrmtMode::Index {
            return;
        }

        let SrcRef::Imm32(selector_value) = sel.src_ref else {
            return;
        };

        let entry = PrmtEntry {
            srcs,
            selector_value,
        };
        self.ssa_prmt.insert(ssa, entry);
    }

    fn try_dedup_srcs(
        &self,
        op: &mut OpPrmt,
        src_idx: usize,
        other_op_entry: &PrmtEntry,
    ) -> bool {
        let SrcRef::Imm32(selector_value) = op.sel.src_ref else {
            return false;
        };

        let other_src_idx = 1 - src_idx;
        let other_src = op.srcs[other_src_idx];

        let mut other_op_target_src_idx = None;

        for (idx, src) in other_op_entry.srcs.iter().enumerate() {
            if *src == other_src {
                other_op_target_src_idx = Some((idx, true));
                break;
            }

            if !is_src_used(other_op_entry.selector_value, idx) {
                other_op_target_src_idx = Some((idx, false));
                break;
            }
        }

        if !is_src_used(selector_value, other_src_idx) {
            other_op_target_src_idx = Some((other_src_idx, true));
        }

        let Some((other_op_target_src_idx, replace_other_src)) = other_op_target_src_idx else {
            return false;
        };

        let other_op_other_src_idx = 1 - other_op_target_src_idx;

        let mut new_selector: u32 = 0;

        for index in 0..4 {
            let (byte_selected, signed_mod) =
                if is_byte_using_src(selector_value, index, src_idx) {
                    let (current_byte_selected, current_signed_mod) =
                        extract_byte_select(selector_value, index);
                    let current_byte_index = current_byte_selected & 0x3;

                    let (target_byte_selected, target_signed_mod) =
                        extract_byte_select(
                            other_op_entry.selector_value,
                            current_byte_index,
                        );

                    let target_src_idx = if is_byte_using_src(
                        other_op_entry.selector_value,
                        current_byte_index,
                        other_op_target_src_idx,
                    ) {
                        other_src_idx
                    } else {
                        src_idx
                    };

                    let target_byte_index = (target_byte_selected & 0x3)
                        + target_src_idx as u32 * 4;

                    (target_byte_index, current_signed_mod | target_signed_mod)
                } else {
                    extract_byte_select(selector_value, index)
                };

            new_selector |= (byte_selected | signed_mod) << (index * 4);
        }

        let new_src = if is_src_used(new_selector, src_idx) {
            other_op_entry.srcs[other_op_other_src_idx]
        } else {
            0.into()
        };

        op.srcs[src_idx] = new_src;

        if replace_other_src {
            let new_other_src = if is_src_used(new_selector, other_src_idx) {
                other_op_entry.srcs[other_op_target_src_idx]
            } else {
                0.into()
            };

            op.srcs[other_src_idx] = new_other_src;
        }

        op.sel = new_selector.into();

        true
    }

    fn try_optimize_selector(&mut self, op: &mut OpPrmt, src_idx: usize) {
        loop {
            let ssa = match op.srcs[src_idx].src_ref {
                SrcRef::SSA(vec) => {
                    assert!(vec.comps() == 1);
                    vec[0]
                }
                _ => return,
            };

            let Some(other_entry) = self.ssa_prmt.get(&ssa) else {
                return;
            };

            if !self.try_dedup_srcs(op, src_idx, other_entry) {
                break;
            }
        }
    }

    fn opt_prmt(&mut self, op: &mut OpPrmt) {
        for i in 0..op.srcs.len() {
            self.try_optimize_selector(op, i);
        }

        if let Dst::SSA(ssa) = op.dst {
            assert!(ssa.comps() == 1);
            self.add_prmt(ssa[0], op.mode, op.srcs, op.sel);
        }
    }

    fn run(&mut self, f: &mut Function) {
        for b in &mut f.blocks {
            for instr in &mut b.instrs {
                if let Op::Prmt(op) = &mut instr.op {
                    self.opt_prmt(op);
                }
            }
        }
    }
}

impl Shader {
    pub fn opt_prmt(&mut self) {
        for f in &mut self.functions {
            PrmtPass::new().run(f);
        }
    }
}

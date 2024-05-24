/*
 * Copyright 2015 Advanced Micro Devices, Inc.
 * Copyright 2024 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AC_DESCRIPTORS_H
#define AC_DESCRIPTORS_H

#include "ac_gpu_info.h"
#include "ac_surface.h"

#include "util/format/u_format.h"

#ifdef __cplusplus
extern "C" {
#endif

unsigned
ac_map_swizzle(unsigned swizzle);

struct ac_sampler_state {
   unsigned address_mode_u : 3;
   unsigned address_mode_v : 3;
   unsigned address_mode_w : 3;
   unsigned max_aniso_ratio : 3;
   unsigned depth_compare_func : 3;
   unsigned unnormalized_coords : 1;
   unsigned cube_wrap : 1;
   unsigned trunc_coord : 1;
   unsigned filter_mode : 2;
   unsigned mag_filter : 2;
   unsigned min_filter : 2;
   unsigned mip_filter : 2;
   unsigned aniso_single_level : 1;
   unsigned border_color_type : 2;
   unsigned border_color_ptr : 12;
   float min_lod;
   float max_lod;
   float lod_bias;
};

void
ac_build_sampler_descriptor(const enum amd_gfx_level gfx_level,
                            const struct ac_sampler_state *state,
                            uint32_t desc[4]);

struct ac_fmask_state {
   const struct radeon_surf *surf;
   uint64_t va;
   uint32_t width : 16;
   uint32_t height : 16;
   uint32_t depth : 14;
   uint32_t type : 4;
   uint32_t first_layer : 14;
   uint32_t last_layer : 13;

   uint32_t num_samples : 5;
   uint32_t num_storage_samples : 4;
   uint32_t tc_compat_cmask : 1;
};

void
ac_build_fmask_descriptor(const enum amd_gfx_level gfx_level,
                          const struct ac_fmask_state *state,
                          uint32_t desc[8]);

uint32_t
ac_tile_mode_index(const struct radeon_surf *surf,
                   unsigned level,
                   bool stencil);

struct ac_mutable_tex_state {
   const struct radeon_surf *surf;
   uint64_t va;

   struct {
      uint32_t write_compress_enable : 1;
      uint32_t iterate_256 : 1;
   } gfx10;

   struct {
      const struct ac_surf_nbc_view *nbc_view;
   } gfx9;

   struct {
      const struct legacy_surf_level *base_level_info;
      uint32_t base_level;
      uint32_t block_width;
   } gfx6;

   uint32_t is_stencil : 1;
   uint32_t dcc_enabled : 1;
   uint32_t tc_compat_htile_enabled : 1;
};

void
ac_set_mutable_tex_desc_fields(const struct radeon_info *info,
                               const struct ac_mutable_tex_state *state,
                               uint32_t desc[8]);

struct ac_buffer_state {
   uint64_t va;
   uint32_t size;
   enum pipe_format format;
   enum pipe_swizzle swizzle[4];
   uint32_t stride;
   uint32_t swizzle_enable : 2;
   uint32_t element_size : 2;
   uint32_t index_stride : 2;
   uint32_t add_tid : 1;
   uint32_t gfx10_oob_select : 2;
};

void
ac_build_buffer_descriptor(const enum amd_gfx_level gfx_level,
                           const struct ac_buffer_state *state,
                           uint32_t desc[4]);

void
ac_build_raw_buffer_descriptor(const enum amd_gfx_level gfx_level,
                               uint64_t va,
                               uint32_t size,
                               uint32_t desc[4]);

void
ac_build_attr_ring_descriptor(const enum amd_gfx_level gfx_level,
                              uint64_t va,
                              uint32_t size,
                              uint32_t desc[4]);

struct ac_ds_state {
   const struct radeon_surf *surf;
   uint64_t va;
   enum pipe_format format;
   uint32_t width : 17;
   uint32_t height : 17;
   uint32_t level : 5;
   uint32_t num_levels : 6;
   uint32_t num_samples : 5;
   uint32_t first_layer : 14;
   uint32_t last_layer : 14;

   uint32_t allow_expclear : 1;
   uint32_t stencil_only : 1;
   uint32_t z_read_only : 1;
   uint32_t stencil_read_only : 1;

   uint32_t htile_enabled : 1;
   uint32_t htile_stencil_disabled : 1;
   uint32_t vrs_enabled : 1;
};

struct ac_ds_surface {
   uint64_t db_depth_base;
   uint64_t db_stencil_base;
   uint32_t db_depth_view;
   uint32_t db_depth_size;
   uint32_t db_z_info;
   uint32_t db_stencil_info;

   union {
      struct {
         uint64_t hiz_base;
         uint32_t hiz_info;
         uint32_t hiz_size_xy;
         uint64_t his_base;
         uint32_t his_info;
         uint32_t his_size_xy;
         uint32_t db_depth_view1;
      } gfx12;

      struct {
         uint64_t db_htile_data_base;
         uint32_t db_depth_info;
         uint32_t db_depth_slice;
         uint32_t db_htile_surface;
         uint32_t db_z_info2;
         uint32_t db_stencil_info2;
      } gfx6;
   } u;
};

void
ac_init_ds_surface(const struct radeon_info *info, const struct ac_ds_state *state, struct ac_ds_surface *ds);

struct ac_mutable_ds_state {
   const struct ac_ds_surface *ds; /* original DS surface */
   enum pipe_format format;
   uint32_t tc_compat_htile_enabled : 1;
   uint32_t zrange_precision : 1;
   uint32_t no_d16_compression : 1;
};

void
ac_set_mutable_ds_surface_fields(const struct radeon_info *info, const struct ac_mutable_ds_state *state,
                                 struct ac_ds_surface *ds);

#ifdef __cplusplus
}
#endif

#endif

/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "vk_log.h"

#include "radv_image_view.h"
#include "radv_buffer_view.h"
#include "radv_entrypoints.h"
#include "radv_formats.h"
#include "radv_image.h"

#include "ac_descriptors.h"
#include "ac_formats.h"
#include "gfx10_format_table.h"

static unsigned
radv_tex_dim(VkImageType image_type, VkImageViewType view_type, unsigned nr_layers, unsigned nr_samples,
             bool is_storage_image, bool gfx9)
{
   if (view_type == VK_IMAGE_VIEW_TYPE_CUBE || view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
      return is_storage_image ? V_008F1C_SQ_RSRC_IMG_2D_ARRAY : V_008F1C_SQ_RSRC_IMG_CUBE;

   /* GFX9 allocates 1D textures as 2D. */
   if (gfx9 && image_type == VK_IMAGE_TYPE_1D)
      image_type = VK_IMAGE_TYPE_2D;
   switch (image_type) {
   case VK_IMAGE_TYPE_1D:
      return nr_layers > 1 ? V_008F1C_SQ_RSRC_IMG_1D_ARRAY : V_008F1C_SQ_RSRC_IMG_1D;
   case VK_IMAGE_TYPE_2D:
      if (nr_samples > 1)
         return nr_layers > 1 ? V_008F1C_SQ_RSRC_IMG_2D_MSAA_ARRAY : V_008F1C_SQ_RSRC_IMG_2D_MSAA;
      else
         return nr_layers > 1 ? V_008F1C_SQ_RSRC_IMG_2D_ARRAY : V_008F1C_SQ_RSRC_IMG_2D;
   case VK_IMAGE_TYPE_3D:
      if (view_type == VK_IMAGE_VIEW_TYPE_3D)
         return V_008F1C_SQ_RSRC_IMG_3D;
      else
         return V_008F1C_SQ_RSRC_IMG_2D_ARRAY;
   default:
      unreachable("illegal image type");
   }
}

void
radv_set_mutable_tex_desc_fields(struct radv_device *device, struct radv_image *image,
                                 const struct legacy_surf_level *base_level_info, unsigned plane_id,
                                 unsigned base_level, unsigned first_level, unsigned block_width, bool is_stencil,
                                 bool is_storage_image, bool disable_compression, bool enable_write_compression,
                                 uint32_t *state, const struct ac_surf_nbc_view *nbc_view)
{
   struct radv_image_plane *plane = &image->planes[plane_id];
   struct radv_image_binding *binding = image->disjoint ? &image->bindings[plane_id] : &image->bindings[0];
   uint64_t gpu_address = binding->bo ? radv_buffer_get_va(binding->bo) + binding->offset : 0;
   const struct radv_physical_device *pdev = radv_device_physical(device);

   const struct ac_mutable_tex_state ac_state = {
      .surf = &plane->surface,
      .va = gpu_address,
      .gfx10 =
         {
            .write_compress_enable =
               radv_dcc_enabled(image, first_level) && is_storage_image && enable_write_compression,
            .iterate_256 = radv_image_get_iterate256(device, image),
         },
      .gfx9 =
         {
            .nbc_view = nbc_view,
         },
      .gfx6 =
         {
            .base_level_info = base_level_info,
            .base_level = base_level,
            .block_width = block_width,
         },
      .is_stencil = is_stencil,
      .dcc_enabled = !disable_compression && radv_dcc_enabled(image, first_level),
      .tc_compat_htile_enabled = !disable_compression && radv_image_is_tc_compat_htile(image),
   };

   ac_set_mutable_tex_desc_fields(&pdev->info, &ac_state, state);
}

/**
 * Build the sampler view descriptor for a texture (GFX10).
 */
static void
gfx10_make_texture_descriptor(struct radv_device *device, struct radv_image *image, bool is_storage_image,
                              VkImageViewType view_type, VkFormat vk_format, const VkComponentMapping *mapping,
                              unsigned first_level, unsigned last_level, unsigned first_layer, unsigned last_layer,
                              unsigned width, unsigned height, unsigned depth, float min_lod, uint32_t *state,
                              uint32_t *fmask_state, VkImageCreateFlags img_create_flags,
                              const struct ac_surf_nbc_view *nbc_view, const VkImageViewSlicedCreateInfoEXT *sliced_3d)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const bool create_2d_view_of_3d =
      (img_create_flags & VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT) && view_type == VK_IMAGE_VIEW_TYPE_2D;
   const struct util_format_description *desc;
   enum pipe_swizzle swizzle[4];
   unsigned array_pitch = 0;
   unsigned img_format;
   unsigned type;

   desc = vk_format_description(vk_format);

   /* For emulated ETC2 without alpha we need to override the format to a 3-componenent format, so
    * that border colors work correctly (alpha forced to 1). Since Vulkan has no such format,
    * this uses the Gallium formats to set the description. */
   if (image->vk.format == VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && vk_format == VK_FORMAT_R8G8B8A8_UNORM) {
      desc = util_format_description(PIPE_FORMAT_R8G8B8X8_UNORM);
   } else if (image->vk.format == VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK && vk_format == VK_FORMAT_R8G8B8A8_SRGB) {
      desc = util_format_description(PIPE_FORMAT_R8G8B8X8_SRGB);
   }

   img_format = ac_get_gfx10_format_table(pdev->info.gfx_level)[vk_format_to_pipe_format(vk_format)].img_format;

   radv_compose_swizzle(desc, mapping, swizzle);

   if (create_2d_view_of_3d) {
      assert(image->vk.image_type == VK_IMAGE_TYPE_3D);
      type = V_008F1C_SQ_RSRC_IMG_3D;
   } else {
      type = radv_tex_dim(image->vk.image_type, view_type, image->vk.array_layers, image->vk.samples, is_storage_image,
                          pdev->info.gfx_level == GFX9);
   }

   if (type == V_008F1C_SQ_RSRC_IMG_1D_ARRAY) {
      height = 1;
      depth = image->vk.array_layers;
   } else if (type == V_008F1C_SQ_RSRC_IMG_2D_ARRAY || type == V_008F1C_SQ_RSRC_IMG_2D_MSAA_ARRAY) {
      if (view_type != VK_IMAGE_VIEW_TYPE_3D)
         depth = image->vk.array_layers;
   } else if (type == V_008F1C_SQ_RSRC_IMG_CUBE)
      depth = image->vk.array_layers / 6;

   if (create_2d_view_of_3d) {
      assert(type == V_008F1C_SQ_RSRC_IMG_3D);

      depth = !is_storage_image ? depth : u_minify(depth, first_level);
      array_pitch = is_storage_image;
   } else if (sliced_3d) {
      assert(type == V_008F1C_SQ_RSRC_IMG_3D && is_storage_image);

      const unsigned total = u_minify(depth, first_level);
      const unsigned slice_count = sliced_3d->sliceCount == VK_REMAINING_3D_SLICES_EXT
                                      ? MAX2(1, total - sliced_3d->sliceOffset)
                                      : sliced_3d->sliceCount;

      first_layer = sliced_3d->sliceOffset;
      depth = sliced_3d->sliceOffset + slice_count;
      array_pitch = 1;
   }

   state[0] = 0;
   state[1] = S_00A004_FORMAT_GFX10(img_format) | S_00A004_WIDTH_LO(width - 1);
   state[2] = S_00A008_WIDTH_HI((width - 1) >> 2) | S_00A008_HEIGHT(height - 1) |
              S_00A008_RESOURCE_LEVEL(pdev->info.gfx_level < GFX11);
   state[3] = S_00A00C_DST_SEL_X(ac_map_swizzle(swizzle[0])) | S_00A00C_DST_SEL_Y(ac_map_swizzle(swizzle[1])) |
              S_00A00C_DST_SEL_Z(ac_map_swizzle(swizzle[2])) | S_00A00C_DST_SEL_W(ac_map_swizzle(swizzle[3])) |
              S_00A00C_BASE_LEVEL(image->vk.samples > 1 ? 0 : first_level) |
              S_00A00C_LAST_LEVEL_GFX10(image->vk.samples > 1 ? util_logbase2(image->vk.samples) : last_level) |
              S_00A00C_BC_SWIZZLE(ac_border_color_swizzle(desc)) | S_00A00C_TYPE(type);
   /* Depth is the the last accessible layer on gfx9+. The hw doesn't need
    * to know the total number of layers.
    */
   state[4] =
      S_00A010_DEPTH_GFX10(type == V_008F1C_SQ_RSRC_IMG_3D ? depth - 1 : last_layer) | S_00A010_BASE_ARRAY(first_layer);
   /* ARRAY_PITCH is only meaningful for 3D images, 0 means SRV, 1 means UAV.
    * In SRV mode, BASE_ARRAY is ignored and DEPTH is the last slice of mipmap level 0.
    * In UAV mode, BASE_ARRAY is the first slice and DEPTH is the last slice of the bound level.
    */
   state[5] = S_00A014_ARRAY_PITCH(array_pitch) | S_00A014_PERF_MOD(4);
   state[6] = 0;
   state[7] = 0;

   unsigned max_mip = image->vk.samples > 1 ? util_logbase2(image->vk.samples) : image->vk.mip_levels - 1;
   if (nbc_view && nbc_view->valid)
      max_mip = nbc_view->num_levels - 1;

   unsigned min_lod_clamped = util_unsigned_fixed(CLAMP(min_lod, 0, 15), 8);
   if (pdev->info.gfx_level >= GFX11) {
      state[1] |= S_00A004_MAX_MIP_GFX11(max_mip);
      state[5] |= S_00A014_MIN_LOD_LO_GFX11(min_lod_clamped);
      state[6] |= S_00A018_MIN_LOD_HI(min_lod_clamped >> 5);
   } else {
      state[1] |= S_00A004_MIN_LOD(min_lod_clamped);
      state[5] |= S_00A014_MAX_MIP(max_mip);
   }

   if (radv_dcc_enabled(image, first_level)) {
      state[6] |=
         S_00A018_MAX_UNCOMPRESSED_BLOCK_SIZE(V_028C78_MAX_BLOCK_SIZE_256B) |
         S_00A018_MAX_COMPRESSED_BLOCK_SIZE(image->planes[0].surface.u.gfx9.color.dcc.max_compressed_block_size) |
         S_00A018_ALPHA_IS_ON_MSB(ac_alpha_is_on_msb(&pdev->info, vk_format_to_pipe_format(vk_format)));
   }

   /* Initialize the sampler view for FMASK. */
   if (fmask_state) {
      if (radv_image_has_fmask(image)) {
         uint64_t gpu_address = radv_buffer_get_va(image->bindings[0].bo);

         assert(image->plane_count == 1);

         const struct ac_fmask_state ac_state = {
            .surf = &image->planes[0].surface,
            .va = gpu_address + image->bindings[0].offset,
            .width = width,
            .height = height,
            .depth = depth,
            .type = radv_tex_dim(image->vk.image_type, view_type, image->vk.array_layers, 0, false, false),
            .first_layer = first_layer,
            .last_layer = last_layer,
            .num_samples = image->vk.samples,
            .num_storage_samples = image->vk.samples,
            .tc_compat_cmask = radv_image_is_tc_compat_cmask(image),
         };

         ac_build_fmask_descriptor(pdev->info.gfx_level, &ac_state, &fmask_state[0]);
      } else
         memset(fmask_state, 0, 8 * 4);
   }
}

/**
 * Build the sampler view descriptor for a texture (SI-GFX9)
 */
static void
gfx6_make_texture_descriptor(struct radv_device *device, struct radv_image *image, bool is_storage_image,
                             VkImageViewType view_type, VkFormat vk_format, const VkComponentMapping *mapping,
                             unsigned first_level, unsigned last_level, unsigned first_layer, unsigned last_layer,
                             unsigned width, unsigned height, unsigned depth, float min_lod, uint32_t *state,
                             uint32_t *fmask_state, VkImageCreateFlags img_create_flags)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_instance *instance = radv_physical_device_instance(pdev);
   const bool create_2d_view_of_3d =
      (img_create_flags & VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT) && view_type == VK_IMAGE_VIEW_TYPE_2D;
   const struct util_format_description *desc;
   enum pipe_swizzle swizzle[4];
   int first_non_void;
   unsigned num_format, data_format, type;

   desc = vk_format_description(vk_format);

   /* For emulated ETC2 without alpha we need to override the format to a 3-componenent format, so
    * that border colors work correctly (alpha forced to 1). Since Vulkan has no such format,
    * this uses the Gallium formats to set the description. */
   if (image->vk.format == VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && vk_format == VK_FORMAT_R8G8B8A8_UNORM) {
      desc = util_format_description(PIPE_FORMAT_R8G8B8X8_UNORM);
   } else if (image->vk.format == VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK && vk_format == VK_FORMAT_R8G8B8A8_SRGB) {
      desc = util_format_description(PIPE_FORMAT_R8G8B8X8_SRGB);
   }

   radv_compose_swizzle(desc, mapping, swizzle);

   first_non_void = vk_format_get_first_non_void_channel(vk_format);

   num_format = radv_translate_tex_numformat(desc, first_non_void);

   data_format = radv_translate_tex_dataformat(pdev, desc, first_non_void);
   if (data_format == ~0) {
      data_format = 0;
   }

   /* S8 with either Z16 or Z32 HTILE need a special format. */
   if (pdev->info.gfx_level == GFX9 && vk_format == VK_FORMAT_S8_UINT && radv_image_is_tc_compat_htile(image)) {
      if (image->vk.format == VK_FORMAT_D32_SFLOAT_S8_UINT)
         data_format = V_008F14_IMG_DATA_FORMAT_S8_32;
      else if (image->vk.format == VK_FORMAT_D16_UNORM_S8_UINT)
         data_format = V_008F14_IMG_DATA_FORMAT_S8_16;
   }

   if (pdev->info.gfx_level == GFX9 && create_2d_view_of_3d) {
      assert(image->vk.image_type == VK_IMAGE_TYPE_3D);
      type = V_008F1C_SQ_RSRC_IMG_3D;
   } else {
      type = radv_tex_dim(image->vk.image_type, view_type, image->vk.array_layers, image->vk.samples, is_storage_image,
                          pdev->info.gfx_level == GFX9);
   }

   if (type == V_008F1C_SQ_RSRC_IMG_1D_ARRAY) {
      height = 1;
      depth = image->vk.array_layers;
   } else if (type == V_008F1C_SQ_RSRC_IMG_2D_ARRAY || type == V_008F1C_SQ_RSRC_IMG_2D_MSAA_ARRAY) {
      if (view_type != VK_IMAGE_VIEW_TYPE_3D)
         depth = image->vk.array_layers;
   } else if (type == V_008F1C_SQ_RSRC_IMG_CUBE)
      depth = image->vk.array_layers / 6;

   state[0] = 0;
   state[1] = (S_008F14_MIN_LOD(util_unsigned_fixed(CLAMP(min_lod, 0, 15), 8)) | S_008F14_DATA_FORMAT(data_format) |
               S_008F14_NUM_FORMAT(num_format));
   state[2] = (S_008F18_WIDTH(width - 1) | S_008F18_HEIGHT(height - 1) | S_008F18_PERF_MOD(4));
   state[3] = (S_008F1C_DST_SEL_X(ac_map_swizzle(swizzle[0])) | S_008F1C_DST_SEL_Y(ac_map_swizzle(swizzle[1])) |
               S_008F1C_DST_SEL_Z(ac_map_swizzle(swizzle[2])) | S_008F1C_DST_SEL_W(ac_map_swizzle(swizzle[3])) |
               S_008F1C_BASE_LEVEL(image->vk.samples > 1 ? 0 : first_level) |
               S_008F1C_LAST_LEVEL(image->vk.samples > 1 ? util_logbase2(image->vk.samples) : last_level) |
               S_008F1C_TYPE(type));
   state[4] = 0;
   state[5] = S_008F24_BASE_ARRAY(first_layer);
   state[6] = 0;
   state[7] = 0;

   if (pdev->info.gfx_level == GFX9) {
      unsigned bc_swizzle = ac_border_color_swizzle(desc);

      /* Depth is the last accessible layer on Gfx9.
       * The hw doesn't need to know the total number of layers.
       */
      if (type == V_008F1C_SQ_RSRC_IMG_3D)
         state[4] |= S_008F20_DEPTH(depth - 1);
      else
         state[4] |= S_008F20_DEPTH(last_layer);

      state[4] |= S_008F20_BC_SWIZZLE(bc_swizzle);
      state[5] |= S_008F24_MAX_MIP(image->vk.samples > 1 ? util_logbase2(image->vk.samples) : image->vk.mip_levels - 1);
   } else {
      state[3] |= S_008F1C_POW2_PAD(image->vk.mip_levels > 1);
      state[4] |= S_008F20_DEPTH(depth - 1);
      state[5] |= S_008F24_LAST_ARRAY(last_layer);
   }

   if (radv_dcc_enabled(image, first_level)) {
      state[6] = S_008F28_ALPHA_IS_ON_MSB(ac_alpha_is_on_msb(&pdev->info, vk_format_to_pipe_format(vk_format)));
   } else {
      if (instance->drirc.disable_aniso_single_level) {
         /* The last dword is unused by hw. The shader uses it to clear
          * bits in the first dword of sampler state.
          */
         if (pdev->info.gfx_level <= GFX7 && image->vk.samples <= 1) {
            if (first_level == last_level)
               state[7] = C_008F30_MAX_ANISO_RATIO;
            else
               state[7] = 0xffffffff;
         }
      }
   }

   /* Initialize the sampler view for FMASK. */
   if (fmask_state) {
      if (radv_image_has_fmask(image)) {
         uint64_t gpu_address = radv_buffer_get_va(image->bindings[0].bo);

         assert(image->plane_count == 1);

         const struct ac_fmask_state ac_state = {
            .surf = &image->planes[0].surface,
            .va = gpu_address + image->bindings[0].offset,
            .width = width,
            .height = height,
            .depth = depth,
            .type = radv_tex_dim(image->vk.image_type, view_type, image->vk.array_layers, 0, false, false),
            .first_layer = first_layer,
            .last_layer = last_layer,
            .num_samples = image->vk.samples,
            .num_storage_samples = image->vk.samples,
            .tc_compat_cmask = radv_image_is_tc_compat_cmask(image),
         };

         ac_build_fmask_descriptor(pdev->info.gfx_level, &ac_state, &fmask_state[0]);
      } else
         memset(fmask_state, 0, 8 * 4);
   }
}

void
radv_make_texture_descriptor(struct radv_device *device, struct radv_image *image, bool is_storage_image,
                             VkImageViewType view_type, VkFormat vk_format, const VkComponentMapping *mapping,
                             unsigned first_level, unsigned last_level, unsigned first_layer, unsigned last_layer,
                             unsigned width, unsigned height, unsigned depth, float min_lod, uint32_t *state,
                             uint32_t *fmask_state, VkImageCreateFlags img_create_flags,
                             const struct ac_surf_nbc_view *nbc_view, const VkImageViewSlicedCreateInfoEXT *sliced_3d)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (pdev->info.gfx_level >= GFX10) {
      gfx10_make_texture_descriptor(device, image, is_storage_image, view_type, vk_format, mapping, first_level,
                                    last_level, first_layer, last_layer, width, height, depth, min_lod, state,
                                    fmask_state, img_create_flags, nbc_view, sliced_3d);
   } else {
      gfx6_make_texture_descriptor(device, image, is_storage_image, view_type, vk_format, mapping, first_level,
                                   last_level, first_layer, last_layer, width, height, depth, min_lod, state,
                                   fmask_state, img_create_flags);
   }
}

static inline void
compute_non_block_compressed_view(struct radv_device *device, const struct radv_image_view *iview,
                                  struct ac_surf_nbc_view *nbc_view)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_image *image = iview->image;
   const struct radeon_surf *surf = &image->planes[0].surface;
   struct ac_addrlib *addrlib = device->ws->get_addrlib(device->ws);
   struct ac_surf_info surf_info = radv_get_ac_surf_info(device, image);

   ac_surface_compute_nbc_view(addrlib, &pdev->info, surf, &surf_info, iview->vk.base_mip_level,
                               iview->vk.base_array_layer, nbc_view);
}

static void
radv_image_view_make_descriptor(struct radv_image_view *iview, struct radv_device *device, VkFormat vk_format,
                                const VkComponentMapping *components, float min_lod, bool is_storage_image,
                                bool disable_compression, bool enable_compression, unsigned plane_id,
                                unsigned descriptor_plane_id, VkImageCreateFlags img_create_flags,
                                const struct ac_surf_nbc_view *nbc_view,
                                const VkImageViewSlicedCreateInfoEXT *sliced_3d, bool force_zero_base_mip)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_image *image = iview->image;
   struct radv_image_plane *plane = &image->planes[plane_id];
   bool is_stencil = iview->vk.aspects == VK_IMAGE_ASPECT_STENCIL_BIT;
   unsigned first_layer = iview->vk.base_array_layer;
   uint32_t blk_w;
   union radv_descriptor *descriptor;
   uint32_t hw_level = iview->vk.base_mip_level;

   if (is_storage_image) {
      descriptor = &iview->storage_descriptor;
   } else {
      descriptor = &iview->descriptor;
   }

   assert(vk_format_get_plane_count(vk_format) == 1);
   assert(plane->surface.blk_w % vk_format_get_blockwidth(plane->format) == 0);
   blk_w = plane->surface.blk_w / vk_format_get_blockwidth(plane->format) * vk_format_get_blockwidth(vk_format);

   if (pdev->info.gfx_level >= GFX9) {
      if (nbc_view->valid) {
         hw_level = nbc_view->level;
         iview->extent.width = nbc_view->width;
         iview->extent.height = nbc_view->height;

         /* Clear the base array layer because addrlib adds it as part of the base addr offset. */
         first_layer = 0;
      }
   } else {
      if (force_zero_base_mip)
         hw_level = 0;
   }

   radv_make_texture_descriptor(device, image, is_storage_image, iview->vk.view_type, vk_format, components, hw_level,
                                hw_level + iview->vk.level_count - 1, first_layer,
                                iview->vk.base_array_layer + iview->vk.layer_count - 1,
                                vk_format_get_plane_width(image->vk.format, plane_id, iview->extent.width),
                                vk_format_get_plane_height(image->vk.format, plane_id, iview->extent.height),
                                iview->extent.depth, min_lod, descriptor->plane_descriptors[descriptor_plane_id],
                                descriptor_plane_id || is_storage_image ? NULL : descriptor->fmask_descriptor,
                                img_create_flags, nbc_view, sliced_3d);

   const struct legacy_surf_level *base_level_info = NULL;
   if (pdev->info.gfx_level <= GFX8) {
      if (is_stencil)
         base_level_info = &plane->surface.u.legacy.zs.stencil_level[iview->vk.base_mip_level];
      else
         base_level_info = &plane->surface.u.legacy.level[force_zero_base_mip ? iview->vk.base_mip_level : 0];
   }

   bool enable_write_compression = radv_image_use_dcc_image_stores(device, image);
   if (is_storage_image && !(enable_write_compression || enable_compression))
      disable_compression = true;
   radv_set_mutable_tex_desc_fields(device, image, base_level_info, plane_id, iview->vk.base_mip_level,
                                    iview->vk.base_mip_level, blk_w, is_stencil, is_storage_image, disable_compression,
                                    enable_write_compression, descriptor->plane_descriptors[descriptor_plane_id],
                                    nbc_view);
}

/**
 * Determine if the given image view can be fast cleared.
 */
static bool
radv_image_view_can_fast_clear(const struct radv_device *device, const struct radv_image_view *iview)
{
   struct radv_image *image;

   if (!iview)
      return false;
   image = iview->image;

   /* Only fast clear if the image itself can be fast cleared. */
   if (!radv_image_can_fast_clear(device, image))
      return false;

   /* Only fast clear if all layers are bound. */
   if (iview->vk.base_array_layer > 0 || iview->vk.layer_count != image->vk.array_layers)
      return false;

   /* Only fast clear if the view covers the whole image. */
   if (!radv_image_extent_compare(image, &iview->extent))
      return false;

   return true;
}

void
radv_image_view_init(struct radv_image_view *iview, struct radv_device *device,
                     const VkImageViewCreateInfo *pCreateInfo, VkImageCreateFlags img_create_flags,
                     const struct radv_image_view_extra_create_info *extra_create_info)
{
   VK_FROM_HANDLE(radv_image, image, pCreateInfo->image);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const VkImageSubresourceRange *range = &pCreateInfo->subresourceRange;
   uint32_t plane_count = 1;
   float min_lod = 0.0f;

   const struct VkImageViewMinLodCreateInfoEXT *min_lod_info =
      vk_find_struct_const(pCreateInfo->pNext, IMAGE_VIEW_MIN_LOD_CREATE_INFO_EXT);

   if (min_lod_info)
      min_lod = min_lod_info->minLod;

   const struct VkImageViewSlicedCreateInfoEXT *sliced_3d =
      vk_find_struct_const(pCreateInfo->pNext, IMAGE_VIEW_SLICED_CREATE_INFO_EXT);

   bool from_client = extra_create_info && extra_create_info->from_client;
   vk_image_view_init(&device->vk, &iview->vk, !from_client, pCreateInfo);

   bool force_zero_base_mip = true;
   if (pdev->info.gfx_level <= GFX8 && min_lod) {
      /* Do not force the base level to zero to workaround a spurious bug with mipmaps and min LOD. */
      force_zero_base_mip = false;
   }

   switch (image->vk.image_type) {
   case VK_IMAGE_TYPE_1D:
   case VK_IMAGE_TYPE_2D:
      assert(range->baseArrayLayer + vk_image_subresource_layer_count(&image->vk, range) - 1 <= image->vk.array_layers);
      break;
   case VK_IMAGE_TYPE_3D:
      assert(range->baseArrayLayer + vk_image_subresource_layer_count(&image->vk, range) - 1 <=
             u_minify(image->vk.extent.depth, range->baseMipLevel));
      break;
   default:
      unreachable("bad VkImageType");
   }
   iview->image = image;
   iview->plane_id = radv_plane_from_aspect(pCreateInfo->subresourceRange.aspectMask);
   iview->nbc_view.valid = false;

   /* If the image has an Android external format, pCreateInfo->format will be
    * VK_FORMAT_UNDEFINED. */
   if (iview->vk.format == VK_FORMAT_UNDEFINED) {
      iview->vk.format = image->vk.format;
      iview->vk.view_format = image->vk.format;
   }

   /* Split out the right aspect. Note that for internal meta code we sometimes
    * use an equivalent color format for the aspect so we first have to check
    * if we actually got depth/stencil formats. */
   if (iview->vk.aspects == VK_IMAGE_ASPECT_STENCIL_BIT) {
      if (vk_format_has_stencil(iview->vk.view_format))
         iview->vk.view_format = vk_format_stencil_only(iview->vk.view_format);
   } else if (iview->vk.aspects == VK_IMAGE_ASPECT_DEPTH_BIT) {
      if (vk_format_has_depth(iview->vk.view_format))
         iview->vk.view_format = vk_format_depth_only(iview->vk.view_format);
   }

   if (vk_format_get_plane_count(image->vk.format) > 1 &&
       pCreateInfo->subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
      plane_count = vk_format_get_plane_count(iview->vk.format);
   }

   /* when the view format is emulated, redirect the view to the hidden plane 1 */
   if (radv_is_format_emulated(pdev, iview->vk.format)) {
      assert(radv_is_format_emulated(pdev, image->vk.format));
      iview->plane_id = 1;
      iview->vk.view_format = image->planes[iview->plane_id].format;
      iview->vk.format = image->planes[iview->plane_id].format;
      plane_count = 1;
   }

   if (!force_zero_base_mip || pdev->info.gfx_level >= GFX9) {
      iview->extent = (VkExtent3D){
         .width = image->vk.extent.width,
         .height = image->vk.extent.height,
         .depth = image->vk.extent.depth,
      };
   } else {
      iview->extent = iview->vk.extent;
   }

   if (iview->vk.format != image->planes[iview->plane_id].format) {
      const struct radv_image_plane *plane = &image->planes[iview->plane_id];
      unsigned view_bw = vk_format_get_blockwidth(iview->vk.format);
      unsigned view_bh = vk_format_get_blockheight(iview->vk.format);
      unsigned plane_bw = vk_format_get_blockwidth(plane->format);
      unsigned plane_bh = vk_format_get_blockheight(plane->format);

      iview->extent.width = DIV_ROUND_UP(iview->extent.width * view_bw, plane_bw);
      iview->extent.height = DIV_ROUND_UP(iview->extent.height * view_bh, plane_bh);

      /* Comment ported from amdvlk -
       * If we have the following image:
       *              Uncompressed pixels   Compressed block sizes (4x4)
       *      mip0:       22 x 22                   6 x 6
       *      mip1:       11 x 11                   3 x 3
       *      mip2:        5 x  5                   2 x 2
       *      mip3:        2 x  2                   1 x 1
       *      mip4:        1 x  1                   1 x 1
       *
       * On GFX9 the descriptor is always programmed with the WIDTH and HEIGHT of the base level and
       * the HW is calculating the degradation of the block sizes down the mip-chain as follows
       * (straight-up divide-by-two integer math): mip0:  6x6 mip1:  3x3 mip2:  1x1 mip3:  1x1
       *
       * This means that mip2 will be missing texels.
       *
       * Fix this by calculating the base mip's width and height, then convert
       * that, and round it back up to get the level 0 size. Clamp the
       * converted size between the original values, and the physical extent
       * of the base mipmap.
       *
       * On GFX10 we have to take care to not go over the physical extent
       * of the base mipmap as otherwise the GPU computes a different layout.
       * Note that the GPU does use the same base-mip dimensions for both a
       * block compatible format and the compressed format, so even if we take
       * the plain converted dimensions the physical layout is correct.
       */
      if (pdev->info.gfx_level >= GFX9 && vk_format_is_block_compressed(plane->format) &&
          !vk_format_is_block_compressed(iview->vk.format)) {
         /* If we have multiple levels in the view we should ideally take the last level,
          * but the mip calculation has a max(..., 1) so walking back to the base mip in an
          * useful way is hard. */
         if (iview->vk.level_count > 1) {
            iview->extent.width = plane->surface.u.gfx9.base_mip_width;
            iview->extent.height = plane->surface.u.gfx9.base_mip_height;
         } else {
            unsigned lvl_width = u_minify(image->vk.extent.width, range->baseMipLevel);
            unsigned lvl_height = u_minify(image->vk.extent.height, range->baseMipLevel);

            lvl_width = DIV_ROUND_UP(lvl_width * view_bw, plane_bw);
            lvl_height = DIV_ROUND_UP(lvl_height * view_bh, plane_bh);

            iview->extent.width =
               CLAMP(lvl_width << range->baseMipLevel, iview->extent.width, plane->surface.u.gfx9.base_mip_width);
            iview->extent.height =
               CLAMP(lvl_height << range->baseMipLevel, iview->extent.height, plane->surface.u.gfx9.base_mip_height);

            /* If the hardware-computed extent is still be too small, on GFX10
             * we can attempt another workaround provided by addrlib that
             * changes the descriptor's base level, and adjusts the address and
             * extents accordingly.
             */
            if (pdev->info.gfx_level >= GFX10 &&
                (u_minify(iview->extent.width, range->baseMipLevel) < lvl_width ||
                 u_minify(iview->extent.height, range->baseMipLevel) < lvl_height) &&
                iview->vk.layer_count == 1) {
               compute_non_block_compressed_view(device, iview, &iview->nbc_view);
            }
         }
      }
   }

   iview->support_fast_clear = radv_image_view_can_fast_clear(device, iview);
   iview->disable_dcc_mrt = extra_create_info ? extra_create_info->disable_dcc_mrt : false;

   bool disable_compression = extra_create_info ? extra_create_info->disable_compression : false;
   bool enable_compression = extra_create_info ? extra_create_info->enable_compression : false;
   for (unsigned i = 0; i < plane_count; ++i) {
      VkFormat format = vk_format_get_plane_format(iview->vk.view_format, i);
      radv_image_view_make_descriptor(iview, device, format, &pCreateInfo->components, min_lod, false,
                                      disable_compression, enable_compression, iview->plane_id + i, i, img_create_flags,
                                      &iview->nbc_view, NULL, force_zero_base_mip);
      radv_image_view_make_descriptor(iview, device, format, &pCreateInfo->components, min_lod, true,
                                      disable_compression, enable_compression, iview->plane_id + i, i, img_create_flags,
                                      &iview->nbc_view, sliced_3d, force_zero_base_mip);
   }
}

void
radv_image_view_finish(struct radv_image_view *iview)
{
   vk_image_view_finish(&iview->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_CreateImageView(VkDevice _device, const VkImageViewCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
   VK_FROM_HANDLE(radv_image, image, pCreateInfo->image);
   VK_FROM_HANDLE(radv_device, device, _device);
   struct radv_image_view *view;

   view = vk_alloc2(&device->vk.alloc, pAllocator, sizeof(*view), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (view == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   radv_image_view_init(view, device, pCreateInfo, image->vk.create_flags,
                        &(struct radv_image_view_extra_create_info){.from_client = true});

   *pView = radv_image_view_to_handle(view);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
radv_DestroyImageView(VkDevice _device, VkImageView _iview, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(radv_device, device, _device);
   VK_FROM_HANDLE(radv_image_view, iview, _iview);

   if (!iview)
      return;

   radv_image_view_finish(iview);
   vk_free2(&device->vk.alloc, pAllocator, iview);
}

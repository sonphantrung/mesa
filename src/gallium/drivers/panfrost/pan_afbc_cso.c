/*
 * Copyright (C) 2023 Amazon.com, Inc. or its affiliates
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "pan_afbc_cso_genx.h"
#include "nir/pipe_nir.h"
#include "nir_builder.h"
#include "pan_context.h"
#include "pan_resource.h"
#include "pan_screen.h"
#include "gen_macros.h"
#include "gen_shaders.h"

#define panfrost_afbc_add_info_ubo(name, b)                                    \
   nir_variable *info_ubo = nir_variable_create(                               \
      b.shader, nir_var_mem_ubo,                                               \
      glsl_array_type(glsl_uint_type(),                                        \
                      sizeof(struct panfrost_afbc_##name##_info) / 4, 0),      \
      "info_ubo");                                                             \
   info_ubo->data.driver_location = 0;

#define panfrost_afbc_get_info_field(name, b, field)                           \
   nir_load_ubo(                                                               \
      (b), 1, sizeof(((struct panfrost_afbc_##name##_info *)0)->field) * 8,    \
      nir_imm_int(b, 0),                                                       \
      nir_imm_int(b, offsetof(struct panfrost_afbc_##name##_info, field)),     \
      .align_mul = 4, .range = ~0)

#define panfrost_afbc_size_get_info_field(b, field)                            \
   panfrost_afbc_get_info_field(size, b, field)

static nir_shader *
panfrost_afbc_create_size_shader(struct panfrost_screen *screen, unsigned bpp,
                                 unsigned align)
{
   struct panfrost_device *dev = pan_device(&screen->base);

   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, screen->vtbl.get_compiler_options(),
      "panfrost_afbc_size(bpp=%d)", bpp);

   panfrost_afbc_add_info_ubo(size, b);

   nir_def *coord = nir_load_global_invocation_id(&b, 32);
   nir_def *block_idx = nir_channel(&b, coord, 0);
   nir_def *src = panfrost_afbc_size_get_info_field(&b, src);
   nir_def *metadata = panfrost_afbc_size_get_info_field(&b, metadata);
   nir_def *uncompressed_size = nir_imm_int(&b, 4 * 4 * bpp / 8); /* bytes */

   nir_def *size = libpan_get_superblock_size(&b, src, block_idx, uncompressed_size);
   size = nir_iand(&b, nir_iadd(&b, size, nir_imm_int(&b, align - 1)),
                   nir_inot(&b, nir_imm_int(&b, align - 1)));

   nir_def *offset = nir_u2u64(
      &b,
      nir_iadd(&b,
               nir_imul_imm(&b, block_idx, sizeof(struct pan_afbc_block_info)),
               nir_imm_int(&b, offsetof(struct pan_afbc_block_info, size))));
   nir_store_global(&b, nir_iadd(&b, metadata, offset), 4, size, 0x1);

   return b.shader;
}

#define panfrost_afbc_pack_get_info_field(b, field)                            \
   panfrost_afbc_get_info_field(pack, b, field)

static nir_shader *
panfrost_afbc_create_pack_shader(struct panfrost_screen *screen, unsigned align,
                                 bool tiled)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, screen->vtbl.get_compiler_options(),
      "panfrost_afbc_pack");

   panfrost_afbc_add_info_ubo(pack, b);

   nir_def *coord = nir_load_global_invocation_id(&b, 32);
   nir_def *src_stride = panfrost_afbc_pack_get_info_field(&b, src_stride);
   nir_def *dst_stride = panfrost_afbc_pack_get_info_field(&b, dst_stride);
   nir_def *dst_idx = nir_channel(&b, coord, 0);
   nir_def *src_idx =
      tiled ? libpan_get_morton_index(&b, dst_idx, src_stride, dst_stride) : dst_idx;
   nir_def *src = panfrost_afbc_pack_get_info_field(&b, src);
   nir_def *dst = panfrost_afbc_pack_get_info_field(&b, dst);
   nir_def *header_size = panfrost_afbc_pack_get_info_field(&b, header_size);
   nir_def *metadata = panfrost_afbc_pack_get_info_field(&b, metadata);

   libpan_copy_superblock(&b, dst, dst_idx, nir_u2u64(&b, header_size),
                          src, src_idx,
                          metadata, src_idx);
   return b.shader;
}

struct pan_afbc_shader_data *
GENX(panfrost_afbc_get_shaders)(struct panfrost_context *ctx,
                          struct panfrost_resource *rsrc, unsigned align)
{
   struct pipe_context *pctx = &ctx->base;
   struct panfrost_screen *screen = pan_screen(ctx->base.screen);
   bool tiled = rsrc->image.layout.modifier & AFBC_FORMAT_MOD_TILED;
   struct pan_afbc_shader_key key = {
      .bpp = util_format_get_blocksizebits(rsrc->base.format),
      .align = align,
      .tiled = tiled,
   };

   pthread_mutex_lock(&ctx->afbc_shaders.lock);
   struct hash_entry *he =
      _mesa_hash_table_search(ctx->afbc_shaders.shaders, &key);
   struct pan_afbc_shader_data *shader = he ? he->data : NULL;
   pthread_mutex_unlock(&ctx->afbc_shaders.lock);

   if (shader)
      return shader;

   shader = rzalloc(ctx->afbc_shaders.shaders, struct pan_afbc_shader_data);
   shader->key = key;
   _mesa_hash_table_insert(ctx->afbc_shaders.shaders, &shader->key, shader);

#define COMPILE_SHADER(name, ...)                                              \
   {                                                                           \
      nir_shader *nir =                                                        \
         panfrost_afbc_create_##name##_shader(screen, __VA_ARGS__);            \
      nir->info.num_ubos = 1;                                                  \
      shader->name##_cso = pipe_shader_from_nir(pctx, nir);                    \
   }

   COMPILE_SHADER(size, key.bpp, key.align);
   COMPILE_SHADER(pack, key.align, key.tiled);

#undef COMPILE_SHADER

   pthread_mutex_lock(&ctx->afbc_shaders.lock);
   _mesa_hash_table_insert(ctx->afbc_shaders.shaders, &shader->key, shader);
   pthread_mutex_unlock(&ctx->afbc_shaders.lock);

   return shader;
}

DERIVE_HASH_TABLE(pan_afbc_shader_key);

void
GENX(panfrost_afbc_context_init)(struct panfrost_context *ctx)
{
   ctx->afbc_shaders.shaders = pan_afbc_shader_key_table_create(NULL);
   pthread_mutex_init(&ctx->afbc_shaders.lock, NULL);
}

void
GENX(panfrost_afbc_context_destroy)(struct panfrost_context *ctx)
{
   _mesa_hash_table_destroy(ctx->afbc_shaders.shaders, NULL);
   pthread_mutex_destroy(&ctx->afbc_shaders.lock);
}

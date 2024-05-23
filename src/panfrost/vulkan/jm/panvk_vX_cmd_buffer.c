/*
 * Copyright © 2024 Collabora Ltd.
 *
 * Derived from tu_cmd_buffer.c which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
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

#include "panvk_buffer.h"
#include "panvk_cmd_buffer.h"
#include "panvk_cmd_desc_state.h"
#include "panvk_cmd_pool.h"
#include "panvk_device.h"
#include "panvk_entrypoints.h"
#include "panvk_event.h"
#include "panvk_image.h"
#include "panvk_image_view.h"
#include "panvk_instance.h"
#include "panvk_physical_device.h"
#include "panvk_pipeline.h"
#include "panvk_priv_bo.h"

#include "pan_blitter.h"
#include "pan_desc.h"
#include "pan_encoder.h"
#include "pan_props.h"
#include "pan_samples.h"
#include "pan_shader.h"

#include "vk_format.h"

static uint32_t
panvk_debug_adjust_bo_flags(const struct panvk_device *device,
                            uint32_t bo_flags)
{
   struct panvk_instance *instance =
      to_panvk_instance(device->vk.physical->instance);

   if (instance->debug_flags & PANVK_DEBUG_DUMP)
      bo_flags &= ~PAN_KMOD_BO_FLAG_NO_MMAP;

   return bo_flags;
}

static void
panvk_cmd_prepare_fragment_job(struct panvk_cmd_buffer *cmdbuf)
{
   const struct pan_fb_info *fbinfo = &cmdbuf->state.gfx.fb.info;
   struct panvk_batch *batch = cmdbuf->cur_batch;
   struct panfrost_ptr job_ptr =
      pan_pool_alloc_desc(&cmdbuf->desc_pool.base, FRAGMENT_JOB);

   GENX(pan_emit_fragment_job)
   (fbinfo, batch->fb.desc.gpu, job_ptr.cpu), batch->fragment_job = job_ptr.gpu;
   util_dynarray_append(&batch->jobs, void *, job_ptr.cpu);
}

void
panvk_per_arch(cmd_close_batch)(struct panvk_cmd_buffer *cmdbuf)
{
   struct panvk_batch *batch = cmdbuf->cur_batch;

   if (!batch)
      return;

   struct pan_fb_info *fbinfo = &cmdbuf->state.gfx.fb.info;

   assert(batch);

   bool clear = fbinfo->zs.clear.z | fbinfo->zs.clear.s;
   for (unsigned i = 0; i < fbinfo->rt_count; i++)
      clear |= fbinfo->rts[i].clear;

   if (!clear && !batch->jc.first_job) {
      if (util_dynarray_num_elements(&batch->event_ops,
                                     struct panvk_cmd_event_op) == 0) {
         /* Content-less batch, let's drop it */
         vk_free(&cmdbuf->vk.pool->alloc, batch);
      } else {
         /* Batch has no jobs but is needed for synchronization, let's add a
          * NULL job so the SUBMIT ioctl doesn't choke on it.
          */
         struct panfrost_ptr ptr =
            pan_pool_alloc_desc(&cmdbuf->desc_pool.base, JOB_HEADER);
         util_dynarray_append(&batch->jobs, void *, ptr.cpu);
         pan_jc_add_job(&cmdbuf->desc_pool.base, &batch->jc, MALI_JOB_TYPE_NULL,
                        false, false, 0, 0, &ptr, false);
         list_addtail(&batch->node, &cmdbuf->batches);
      }
      cmdbuf->cur_batch = NULL;
      return;
   }

   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(dev->vk.physical);

   list_addtail(&batch->node, &cmdbuf->batches);

   if (batch->jc.first_tiler) {
      struct panfrost_ptr preload_jobs[2];
      unsigned num_preload_jobs = GENX(pan_preload_fb)(
         &dev->meta.blitter.cache, &cmdbuf->desc_pool.base, &batch->jc,
         &cmdbuf->state.gfx.fb.info, batch->tls.gpu, batch->tiler.ctx_desc.gpu,
         preload_jobs);
      for (unsigned i = 0; i < num_preload_jobs; i++)
         util_dynarray_append(&batch->jobs, void *, preload_jobs[i].cpu);
   }

   if (batch->tlsinfo.tls.size) {
      unsigned thread_tls_alloc =
         panfrost_query_thread_tls_alloc(&phys_dev->kmod.props);
      unsigned core_id_range;

      panfrost_query_core_count(&phys_dev->kmod.props, &core_id_range);

      unsigned size = panfrost_get_total_stack_size(
         batch->tlsinfo.tls.size, thread_tls_alloc, core_id_range);
      batch->tlsinfo.tls.ptr =
         pan_pool_alloc_aligned(&cmdbuf->tls_pool.base, size, 4096).gpu;
   }

   if (batch->tlsinfo.wls.size) {
      assert(batch->wls_total_size);
      batch->tlsinfo.wls.ptr =
         pan_pool_alloc_aligned(&cmdbuf->tls_pool.base, batch->wls_total_size,
                                4096)
            .gpu;
   }

   if (batch->tls.cpu)
      GENX(pan_emit_tls)(&batch->tlsinfo, batch->tls.cpu);

   if (batch->fb.desc.cpu) {
      fbinfo->sample_positions = dev->sample_positions->addr.dev +
                                 panfrost_sample_positions_offset(
                                    pan_sample_pattern(fbinfo->nr_samples));

      batch->fb.desc.gpu |=
         GENX(pan_emit_fbd)(&cmdbuf->state.gfx.fb.info, &batch->tlsinfo,
                            &batch->tiler.ctx, batch->fb.desc.cpu);

      panvk_cmd_prepare_fragment_job(cmdbuf);
   }

   cmdbuf->cur_batch = NULL;
}

void
panvk_per_arch(cmd_alloc_fb_desc)(struct panvk_cmd_buffer *cmdbuf)
{
   struct panvk_batch *batch = cmdbuf->cur_batch;

   if (batch->fb.desc.gpu)
      return;

   const struct pan_fb_info *fbinfo = &cmdbuf->state.gfx.fb.info;
   bool has_zs_ext = fbinfo->zs.view.zs || fbinfo->zs.view.s;

   batch->fb.bo_count = cmdbuf->state.gfx.fb.bo_count;
   memcpy(batch->fb.bos, cmdbuf->state.gfx.fb.bos,
          batch->fb.bo_count * sizeof(batch->fb.bos[0]));
   batch->fb.desc = pan_pool_alloc_desc_aggregate(
      &cmdbuf->desc_pool.base, PAN_DESC(FRAMEBUFFER),
      PAN_DESC_ARRAY(has_zs_ext ? 1 : 0, ZS_CRC_EXTENSION),
      PAN_DESC_ARRAY(MAX2(fbinfo->rt_count, 1), RENDER_TARGET));

   memset(&cmdbuf->state.gfx.fb.info.bifrost.pre_post.dcds, 0,
          sizeof(cmdbuf->state.gfx.fb.info.bifrost.pre_post.dcds));
}

void
panvk_per_arch(cmd_alloc_tls_desc)(struct panvk_cmd_buffer *cmdbuf, bool gfx)
{
   struct panvk_batch *batch = cmdbuf->cur_batch;

   assert(batch);
   if (!batch->tls.gpu) {
      batch->tls = pan_pool_alloc_desc(&cmdbuf->desc_pool.base, LOCAL_STORAGE);
   }
}

void
panvk_per_arch(cmd_get_tiler_context)(struct panvk_cmd_buffer *cmdbuf,
                                      unsigned width, unsigned height)
{
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct pan_fb_info *fbinfo = &cmdbuf->state.gfx.fb.info;
   struct panvk_batch *batch = cmdbuf->cur_batch;

   if (batch->tiler.ctx_desc.cpu)
      return;

   batch->tiler.heap_desc =
      pan_pool_alloc_desc(&cmdbuf->desc_pool.base, TILER_HEAP);
   batch->tiler.ctx_desc =
      pan_pool_alloc_desc(&cmdbuf->desc_pool.base, TILER_CONTEXT);

   pan_pack(&batch->tiler.heap_templ, TILER_HEAP, cfg) {
      cfg.size = pan_kmod_bo_size(dev->tiler_heap->bo);
      cfg.base = dev->tiler_heap->addr.dev;
      cfg.bottom = dev->tiler_heap->addr.dev;
      cfg.top = cfg.base + cfg.size;
   }

   pan_pack(&batch->tiler.ctx_templ, TILER_CONTEXT, cfg) {
      cfg.hierarchy_mask = 0x28;
      cfg.fb_width = width;
      cfg.fb_height = height;
      cfg.heap = batch->tiler.heap_desc.gpu;
      cfg.sample_pattern = pan_sample_pattern(fbinfo->nr_samples);
   }

   memcpy(batch->tiler.heap_desc.cpu, &batch->tiler.heap_templ,
          sizeof(batch->tiler.heap_templ));
   memcpy(batch->tiler.ctx_desc.cpu, &batch->tiler.ctx_templ,
          sizeof(batch->tiler.ctx_templ));
   batch->tiler.ctx.bifrost = batch->tiler.ctx_desc.gpu;
}

void
panvk_per_arch(cmd_prepare_tiler_context)(struct panvk_cmd_buffer *cmdbuf)
{
   const struct pan_fb_info *fbinfo = &cmdbuf->state.gfx.fb.info;

   panvk_per_arch(cmd_get_tiler_context)(cmdbuf, fbinfo->width, fbinfo->height);
}

void
panvk_per_arch(cmd_preload_fb_after_batch_split)(struct panvk_cmd_buffer *cmdbuf)
{
   for (unsigned i = 0; i < cmdbuf->state.gfx.fb.info.rt_count; i++) {
      if (cmdbuf->state.gfx.fb.info.rts[i].view) {
         cmdbuf->state.gfx.fb.info.rts[i].clear = false;
         cmdbuf->state.gfx.fb.info.rts[i].preload = true;
      }
   }

   if (cmdbuf->state.gfx.fb.info.zs.view.zs) {
      cmdbuf->state.gfx.fb.info.zs.clear.z = false;
      cmdbuf->state.gfx.fb.info.zs.preload.z = true;
   }

   if (cmdbuf->state.gfx.fb.info.zs.view.s ||
       (cmdbuf->state.gfx.fb.info.zs.view.zs &&
        util_format_is_depth_and_stencil(
           cmdbuf->state.gfx.fb.info.zs.view.zs->format))) {
      cmdbuf->state.gfx.fb.info.zs.clear.s = false;
      cmdbuf->state.gfx.fb.info.zs.preload.s = true;
   }
}

struct panvk_batch *
panvk_per_arch(cmd_open_batch)(struct panvk_cmd_buffer *cmdbuf)
{
   assert(!cmdbuf->cur_batch);
   cmdbuf->cur_batch =
      vk_zalloc(&cmdbuf->vk.pool->alloc, sizeof(*cmdbuf->cur_batch), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   util_dynarray_init(&cmdbuf->cur_batch->jobs, NULL);
   util_dynarray_init(&cmdbuf->cur_batch->event_ops, NULL);
   assert(cmdbuf->cur_batch);
   return cmdbuf->cur_batch;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_per_arch(EndCommandBuffer)(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   panvk_per_arch(cmd_close_batch)(cmdbuf);

   return vk_command_buffer_end(&cmdbuf->vk);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdPipelineBarrier2)(VkCommandBuffer commandBuffer,
                                    const VkDependencyInfo *pDependencyInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   /* Caches are flushed/invalidated at batch boundaries for now, nothing to do
    * for memory barriers assuming we implement barriers with the creation of a
    * new batch.
    * FIXME: We can probably do better with a CacheFlush job that has the
    * barrier flag set to true.
    */
   if (cmdbuf->cur_batch) {
      panvk_per_arch(cmd_close_batch)(cmdbuf);
      panvk_per_arch(cmd_preload_fb_after_batch_split)(cmdbuf);
      panvk_per_arch(cmd_open_batch)(cmdbuf);
   }
}

static void
panvk_add_set_event_operation(struct panvk_cmd_buffer *cmdbuf,
                              struct panvk_event *event,
                              enum panvk_cmd_event_op_type type)
{
   struct panvk_cmd_event_op op = {
      .type = type,
      .event = event,
   };

   if (cmdbuf->cur_batch == NULL) {
      /* No open batch, let's create a new one so this operation happens in
       * the right order.
       */
      panvk_per_arch(cmd_open_batch)(cmdbuf);
      util_dynarray_append(&cmdbuf->cur_batch->event_ops,
                           struct panvk_cmd_event_op, op);
      panvk_per_arch(cmd_close_batch)(cmdbuf);
   } else {
      /* Let's close the current batch so the operation executes before any
       * future commands.
       */
      util_dynarray_append(&cmdbuf->cur_batch->event_ops,
                           struct panvk_cmd_event_op, op);
      panvk_per_arch(cmd_close_batch)(cmdbuf);
      panvk_per_arch(cmd_preload_fb_after_batch_split)(cmdbuf);
      panvk_per_arch(cmd_open_batch)(cmdbuf);
   }
}

static void
panvk_add_wait_event_operation(struct panvk_cmd_buffer *cmdbuf,
                               struct panvk_event *event)
{
   struct panvk_cmd_event_op op = {
      .type = PANVK_EVENT_OP_WAIT,
      .event = event,
   };

   if (cmdbuf->cur_batch == NULL) {
      /* No open batch, let's create a new one and have it wait for this event. */
      panvk_per_arch(cmd_open_batch)(cmdbuf);
      util_dynarray_append(&cmdbuf->cur_batch->event_ops,
                           struct panvk_cmd_event_op, op);
   } else {
      /* Let's close the current batch so any future commands wait on the
       * event signal operation.
       */
      if (cmdbuf->cur_batch->fragment_job || cmdbuf->cur_batch->jc.first_job) {
         panvk_per_arch(cmd_close_batch)(cmdbuf);
         panvk_per_arch(cmd_preload_fb_after_batch_split)(cmdbuf);
         panvk_per_arch(cmd_open_batch)(cmdbuf);
      }
      util_dynarray_append(&cmdbuf->cur_batch->event_ops,
                           struct panvk_cmd_event_op, op);
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdSetEvent2)(VkCommandBuffer commandBuffer, VkEvent _event,
                             const VkDependencyInfo *pDependencyInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_event, event, _event);

   /* vkCmdSetEvent cannot be called inside a render pass */
   assert(cmdbuf->vk.render_pass == NULL);

   panvk_add_set_event_operation(cmdbuf, event, PANVK_EVENT_OP_SET);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdResetEvent2)(VkCommandBuffer commandBuffer, VkEvent _event,
                               VkPipelineStageFlags2 stageMask)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_event, event, _event);

   /* vkCmdResetEvent cannot be called inside a render pass */
   assert(cmdbuf->vk.render_pass == NULL);

   panvk_add_set_event_operation(cmdbuf, event, PANVK_EVENT_OP_RESET);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdWaitEvents2)(VkCommandBuffer commandBuffer,
                               uint32_t eventCount, const VkEvent *pEvents,
                               const VkDependencyInfo *pDependencyInfos)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   assert(eventCount > 0);

   for (uint32_t i = 0; i < eventCount; i++) {
      VK_FROM_HANDLE(panvk_event, event, pEvents[i]);
      panvk_add_wait_event_operation(cmdbuf, event);
   }
}

static void
panvk_reset_cmdbuf(struct vk_command_buffer *vk_cmdbuf,
                   VkCommandBufferResetFlags flags)
{
   struct panvk_cmd_buffer *cmdbuf =
      container_of(vk_cmdbuf, struct panvk_cmd_buffer, vk);

   vk_command_buffer_reset(&cmdbuf->vk);

   list_for_each_entry_safe(struct panvk_batch, batch, &cmdbuf->batches, node) {
      list_del(&batch->node);
      util_dynarray_fini(&batch->jobs);
      util_dynarray_fini(&batch->event_ops);

      vk_free(&cmdbuf->vk.pool->alloc, batch);
   }

   panvk_pool_reset(&cmdbuf->desc_pool);
   panvk_pool_reset(&cmdbuf->tls_pool);
   panvk_pool_reset(&cmdbuf->varying_pool);

   panvk_per_arch(cmd_desc_state_reset)(cmdbuf);
}

static void
panvk_destroy_cmdbuf(struct vk_command_buffer *vk_cmdbuf)
{
   struct panvk_cmd_buffer *cmdbuf =
      container_of(vk_cmdbuf, struct panvk_cmd_buffer, vk);
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);

   panvk_per_arch(cmd_desc_state_cleanup)(cmdbuf);

   list_for_each_entry_safe(struct panvk_batch, batch, &cmdbuf->batches, node) {
      list_del(&batch->node);
      util_dynarray_fini(&batch->jobs);
      util_dynarray_fini(&batch->event_ops);

      vk_free(&cmdbuf->vk.pool->alloc, batch);
   }

   panvk_pool_cleanup(&cmdbuf->desc_pool);
   panvk_pool_cleanup(&cmdbuf->tls_pool);
   panvk_pool_cleanup(&cmdbuf->varying_pool);
   vk_command_buffer_finish(&cmdbuf->vk);
   vk_free(&dev->vk.alloc, cmdbuf);
}

static VkResult
panvk_create_cmdbuf(struct vk_command_pool *vk_pool, VkCommandBufferLevel level,
                    struct vk_command_buffer **cmdbuf_out)
{
   struct panvk_device *device =
      container_of(vk_pool->base.device, struct panvk_device, vk);
   struct panvk_cmd_pool *pool =
      container_of(vk_pool, struct panvk_cmd_pool, vk);
   struct panvk_cmd_buffer *cmdbuf;

   cmdbuf = vk_zalloc(&device->vk.alloc, sizeof(*cmdbuf), 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmdbuf)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = vk_command_buffer_init(
      &pool->vk, &cmdbuf->vk, &panvk_per_arch(cmd_buffer_ops), level);
   if (result != VK_SUCCESS) {
      vk_free(&device->vk.alloc, cmdbuf);
      return result;
   }

   cmdbuf->vk.dynamic_graphics_state.vi = &cmdbuf->state.gfx.dynamic.vi;
   cmdbuf->vk.dynamic_graphics_state.ms.sample_locations =
      &cmdbuf->state.gfx.dynamic.sl;

   panvk_pool_init(&cmdbuf->desc_pool, device, &pool->desc_bo_pool, 0,
                   64 * 1024, "Command buffer descriptor pool", true);
   panvk_pool_init(
      &cmdbuf->tls_pool, device, &pool->tls_bo_pool,
      panvk_debug_adjust_bo_flags(device, PAN_KMOD_BO_FLAG_NO_MMAP), 64 * 1024,
      "TLS pool", false);
   panvk_pool_init(
      &cmdbuf->varying_pool, device, &pool->varying_bo_pool,
      panvk_debug_adjust_bo_flags(device, PAN_KMOD_BO_FLAG_NO_MMAP), 64 * 1024,
      "Varyings pool", false);
   list_inithead(&cmdbuf->batches);
   *cmdbuf_out = &cmdbuf->vk;
   return VK_SUCCESS;
}

const struct vk_command_buffer_ops panvk_per_arch(cmd_buffer_ops) = {
   .create = panvk_create_cmdbuf,
   .reset = panvk_reset_cmdbuf,
   .destroy = panvk_destroy_cmdbuf,
};

VKAPI_ATTR VkResult VKAPI_CALL
panvk_per_arch(BeginCommandBuffer)(VkCommandBuffer commandBuffer,
                                   const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   vk_command_buffer_begin(&cmdbuf->vk, pBeginInfo);

   memset(&cmdbuf->state, 0, sizeof(cmdbuf->state));

   return VK_SUCCESS;
}

static void
panvk_cmd_begin_rendering_init_fbinfo(struct panvk_cmd_buffer *cmdbuf,
                                      const VkRenderingInfo *pRenderingInfo)
{
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(dev->vk.physical);
   struct pan_fb_info *fbinfo = &cmdbuf->state.gfx.fb.info;
   uint32_t att_width = 0, att_height = 0;

   cmdbuf->state.gfx.fb.bo_count = 0;
   memset(cmdbuf->state.gfx.fb.bos, 0, sizeof(cmdbuf->state.gfx.fb.bos));
   memset(cmdbuf->state.gfx.fb.crc_valid, 0,
          sizeof(cmdbuf->state.gfx.fb.crc_valid));
   memset(&cmdbuf->state.gfx.fb.color_attachments, 0,
          sizeof(cmdbuf->state.gfx.fb.color_attachments));
   cmdbuf->state.gfx.fb.bound_attachments = 0;

   *fbinfo = (struct pan_fb_info){
      .tile_buf_budget = panfrost_query_optimal_tib_size(phys_dev->model),
      .nr_samples = 1,
      .rt_count = pRenderingInfo->colorAttachmentCount,
   };

   assert(pRenderingInfo->colorAttachmentCount <= ARRAY_SIZE(fbinfo->rts));

   for (uint32_t i = 0; i < pRenderingInfo->colorAttachmentCount; i++) {
      const VkRenderingAttachmentInfo *att =
         &pRenderingInfo->pColorAttachments[i];
      VK_FROM_HANDLE(panvk_image_view, iview, att->imageView);

      if (!iview)
         continue;

      struct panvk_image *img =
         container_of(iview->vk.image, struct panvk_image, vk);
      const VkExtent3D iview_size =
         vk_image_mip_level_extent(&img->vk, iview->vk.base_mip_level);

      cmdbuf->state.gfx.fb.bound_attachments |=
         MESA_VK_RP_ATTACHMENT_COLOR_BIT(i);
      cmdbuf->state.gfx.fb.color_attachments.fmts[i] = iview->vk.format;
      cmdbuf->state.gfx.fb.color_attachments.samples[i] = img->vk.samples;
      att_width = MAX2(iview_size.width, att_width);
      att_height = MAX2(iview_size.height, att_height);

      assert(att->resolveMode == VK_RESOLVE_MODE_NONE);

      cmdbuf->state.gfx.fb.bos[cmdbuf->state.gfx.fb.bo_count++] = img->bo;
      fbinfo->rts[i].view = &iview->pview;
      fbinfo->rts[i].crc_valid = &cmdbuf->state.gfx.fb.crc_valid[i];
      fbinfo->nr_samples =
         MAX2(fbinfo->nr_samples, pan_image_view_get_nr_samples(&iview->pview));

      if (att->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
         enum pipe_format fmt = vk_format_to_pipe_format(iview->vk.format);
         union pipe_color_union *col =
            (union pipe_color_union *)&att->clearValue.color;

         fbinfo->rts[i].clear = true;
         pan_pack_color(phys_dev->formats.blendable, fbinfo->rts[i].clear_value,
                        col, fmt, false);
      } else if (att->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
         fbinfo->rts[i].preload = true;
      }
   }

   if (pRenderingInfo->pDepthAttachment &&
       pRenderingInfo->pDepthAttachment->imageView != VK_NULL_HANDLE) {
      const VkRenderingAttachmentInfo *att = pRenderingInfo->pDepthAttachment;
      VK_FROM_HANDLE(panvk_image_view, iview, att->imageView);
      struct panvk_image *img =
         container_of(iview->vk.image, struct panvk_image, vk);
      const VkExtent3D iview_size =
         vk_image_mip_level_extent(&img->vk, iview->vk.base_mip_level);

      cmdbuf->state.gfx.fb.bound_attachments |= MESA_VK_RP_ATTACHMENT_DEPTH_BIT;
      att_width = MAX2(iview_size.width, att_width);
      att_height = MAX2(iview_size.height, att_height);

      assert(att->resolveMode == VK_RESOLVE_MODE_NONE);

      cmdbuf->state.gfx.fb.bos[cmdbuf->state.gfx.fb.bo_count++] = img->bo;
      fbinfo->zs.view.zs = &iview->pview;

      if (att->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
         fbinfo->zs.clear.z = true;
         fbinfo->zs.clear_value.depth = att->clearValue.depthStencil.depth;
      } else if (att->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
         fbinfo->zs.preload.z = true;
      }
   }

   if (pRenderingInfo->pStencilAttachment &&
       pRenderingInfo->pStencilAttachment->imageView != VK_NULL_HANDLE) {
      const VkRenderingAttachmentInfo *att = pRenderingInfo->pStencilAttachment;
      VK_FROM_HANDLE(panvk_image_view, iview, att->imageView);
      struct panvk_image *img =
         container_of(iview->vk.image, struct panvk_image, vk);
      const VkExtent3D iview_size =
         vk_image_mip_level_extent(&img->vk, iview->vk.base_mip_level);

      cmdbuf->state.gfx.fb.bound_attachments |=
         MESA_VK_RP_ATTACHMENT_STENCIL_BIT;
      att_width = MAX2(iview_size.width, att_width);
      att_height = MAX2(iview_size.height, att_height);

      assert(att->resolveMode == VK_RESOLVE_MODE_NONE);

      cmdbuf->state.gfx.fb.bos[cmdbuf->state.gfx.fb.bo_count++] = img->bo;
      fbinfo->zs.view.s =
         &iview->pview != fbinfo->zs.view.zs ? &iview->pview : NULL;

      if (att->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
         fbinfo->zs.clear.s = true;
         fbinfo->zs.clear_value.stencil = att->clearValue.depthStencil.stencil;
      } else if (att->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
         fbinfo->zs.preload.s = true;
      }
   }

   fbinfo->width = pRenderingInfo->renderArea.offset.x +
                   pRenderingInfo->renderArea.extent.width;
   fbinfo->height = pRenderingInfo->renderArea.offset.y +
                    pRenderingInfo->renderArea.extent.height;

   if (cmdbuf->state.gfx.fb.bound_attachments) {
      /* We need the rendering area to be aligned on a 32x32 section for tile
       * buffer preloading to work correctly.
       */
      fbinfo->width = MIN2(att_width, ALIGN_POT(fbinfo->width, 32));
      fbinfo->height = MIN2(att_height, ALIGN_POT(fbinfo->height, 32));
   }

   assert(fbinfo->width && fbinfo->height);

   fbinfo->extent.maxx = fbinfo->width - 1;
   fbinfo->extent.maxy = fbinfo->height - 1;

   /* We need to re-emit the FS RSD when the color attachments change. */
   cmdbuf->state.gfx.fs.rsd = 0;
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdBeginRendering)(VkCommandBuffer commandBuffer,
                                  const VkRenderingInfo *pRenderingInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   panvk_cmd_begin_rendering_init_fbinfo(cmdbuf, pRenderingInfo);
   panvk_per_arch(cmd_open_batch)(cmdbuf);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdEndRendering)(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   panvk_per_arch(cmd_close_batch)(cmdbuf);
   cmdbuf->cur_batch = NULL;
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdBindVertexBuffers)(VkCommandBuffer commandBuffer,
                                     uint32_t firstBinding,
                                     uint32_t bindingCount,
                                     const VkBuffer *pBuffers,
                                     const VkDeviceSize *pOffsets)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   assert(firstBinding + bindingCount <= MAX_VBS);

   for (uint32_t i = 0; i < bindingCount; i++) {
      VK_FROM_HANDLE(panvk_buffer, buffer, pBuffers[i]);

      cmdbuf->state.gfx.vb.bufs[firstBinding + i].address =
         panvk_buffer_gpu_ptr(buffer, pOffsets[i]);
      cmdbuf->state.gfx.vb.bufs[firstBinding + i].size =
         panvk_buffer_range(buffer, pOffsets[i], VK_WHOLE_SIZE);
   }

   cmdbuf->state.gfx.vb.count =
      MAX2(cmdbuf->state.gfx.vb.count, firstBinding + bindingCount);
   cmdbuf->state.gfx.vs.attrib_bufs = 0;
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdBindIndexBuffer)(VkCommandBuffer commandBuffer,
                                   VkBuffer buffer, VkDeviceSize offset,
                                   VkIndexType indexType)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_buffer, buf, buffer);

   cmdbuf->state.gfx.ib.buffer = buf;
   cmdbuf->state.gfx.ib.offset = offset;
   switch (indexType) {
   case VK_INDEX_TYPE_UINT16:
      cmdbuf->state.gfx.ib.index_size = 16;
      break;
   case VK_INDEX_TYPE_UINT32:
      cmdbuf->state.gfx.ib.index_size = 32;
      break;
   case VK_INDEX_TYPE_NONE_KHR:
      cmdbuf->state.gfx.ib.index_size = 0;
      break;
   case VK_INDEX_TYPE_UINT8_EXT:
      cmdbuf->state.gfx.ib.index_size = 8;
      break;
   default:
      unreachable("Invalid index type\n");
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdBindPipeline)(VkCommandBuffer commandBuffer,
                                VkPipelineBindPoint pipelineBindPoint,
                                VkPipeline _pipeline)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_pipeline, pipeline, _pipeline);

   switch (pipelineBindPoint) {
   case VK_PIPELINE_BIND_POINT_GRAPHICS: {
      struct panvk_graphics_pipeline *gfx_pipeline =
         panvk_pipeline_to_graphics_pipeline(pipeline);

      vk_cmd_set_dynamic_graphics_state(&cmdbuf->vk,
                                        &gfx_pipeline->state.dynamic);

      cmdbuf->state.gfx.fs.rsd = 0;
      cmdbuf->state.gfx.pipeline = gfx_pipeline;
      break;
   }

   case VK_PIPELINE_BIND_POINT_COMPUTE:
      cmdbuf->state.compute.pipeline =
         panvk_pipeline_to_compute_pipeline(pipeline);
      break;

   default:
      assert(!"Unsupported bind point");
      break;
   }
}

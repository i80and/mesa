/*
 * Copyright © 2017 Broadcom
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

#include "util/u_format.h"
#include "vc5_context.h"
#include "vc5_tiling.h"
#include "broadcom/cle/v3d_packet_v33_pack.h"

static void
load_raw(struct vc5_cl *cl, struct pipe_surface *psurf, int buffer)
{
        struct vc5_surface *surf = vc5_surface(psurf);
        struct vc5_resource *rsc = vc5_resource(psurf->texture);

        cl_emit(cl, LOAD_TILE_BUFFER_GENERAL, load) {
                load.raw_mode = true;
                load.buffer_to_load = buffer;
                load.address = cl_address(rsc->bo, surf->offset);
                load.padded_height_of_output_image_in_uif_blocks =
                        surf->padded_height_of_output_image_in_uif_blocks;
        }
}

static void
store_raw(struct vc5_cl *cl, struct pipe_surface *psurf, int buffer,
          bool color_clear, bool z_clear, bool s_clear)
{
        struct vc5_surface *surf = vc5_surface(psurf);
        struct vc5_resource *rsc = vc5_resource(psurf->texture);

        cl_emit(cl, STORE_TILE_BUFFER_GENERAL, store) {
                store.raw_mode = true;
                store.buffer_to_store = buffer;
                store.address = cl_address(rsc->bo, surf->offset);
                store.disable_colour_buffers_clear_on_write = !color_clear;
                store.disable_z_buffer_clear_on_write = !z_clear;
                store.disable_stencil_buffer_clear_on_write = !s_clear;
                store.padded_height_of_output_image_in_uif_blocks =
                        surf->padded_height_of_output_image_in_uif_blocks;
        }
}

static int
zs_buffer_from_pipe_bits(int pipe_clear_bits)
{
        switch (pipe_clear_bits & PIPE_CLEAR_DEPTHSTENCIL) {
        case PIPE_CLEAR_DEPTHSTENCIL:
                return ZSTENCIL;
        case PIPE_CLEAR_DEPTH:
                return Z;
        case PIPE_CLEAR_STENCIL:
                return STENCIL;
        default:
                return NONE;
        }
}

/* The HW queues up the load until the tile coordinates show up, but can only
 * track one at a time.  If we need to do more than one load, then we need to
 * flush out the previous load by emitting the tile coordinates and doing a
 * dummy store.
 */
static void
flush_last_load(struct vc5_cl *cl)
{
        cl_emit(cl, TILE_COORDINATES_IMPLICIT, coords);
        cl_emit(cl, STORE_TILE_BUFFER_GENERAL, store) {
                store.buffer_to_store = NONE;
        }
}

static void
vc5_rcl_emit_generic_per_tile_list(struct vc5_job *job, int last_cbuf)
{
        /* Emit the generic list in our indirect state -- the rcl will just
         * have pointers into it.
         */
        struct vc5_cl *cl = &job->indirect;
        vc5_cl_ensure_space(cl, 200, 1);
        struct vc5_cl_reloc tile_list_start = cl_get_address(cl);

        const uint32_t pipe_clear_color_buffers = (PIPE_CLEAR_COLOR0 |
                                                   PIPE_CLEAR_COLOR1 |
                                                   PIPE_CLEAR_COLOR2 |
                                                   PIPE_CLEAR_COLOR3);
        const uint32_t first_color_buffer_bit = (ffs(PIPE_CLEAR_COLOR0) - 1);

        uint32_t read_but_not_cleared = job->resolve & ~job->cleared;

        for (int i = 0; i < VC5_MAX_DRAW_BUFFERS; i++) {
                uint32_t bit = PIPE_CLEAR_COLOR0 << i;
                if (!(read_but_not_cleared & bit))
                        continue;

                struct pipe_surface *psurf = job->cbufs[i];
                if (!psurf || psurf->texture->nr_samples <= 1)
                        continue;

                load_raw(cl, psurf, RENDER_TARGET_0 + i);
                read_but_not_cleared &= ~bit;

                if (read_but_not_cleared)
                        flush_last_load(cl);
        }

        if (job->zsbuf && job->zsbuf->texture->nr_samples > 1 &&
            read_but_not_cleared & PIPE_CLEAR_DEPTHSTENCIL) {
                load_raw(cl, job->zsbuf,
                         zs_buffer_from_pipe_bits(read_but_not_cleared));
                read_but_not_cleared &= ~PIPE_CLEAR_DEPTHSTENCIL;
                if (read_but_not_cleared)
                        cl_emit(cl, TILE_COORDINATES_IMPLICIT, coords);
        }

        /* The initial reload will be queued until we get the
         * tile coordinates.
         */
        if (read_but_not_cleared) {
                cl_emit(cl, RELOAD_TILE_COLOUR_BUFFER, load) {
                        load.disable_colour_buffer_load =
                                (~read_but_not_cleared & pipe_clear_color_buffers) >>
                                first_color_buffer_bit;
                        load.enable_z_load =
                                read_but_not_cleared & PIPE_CLEAR_DEPTH;
                        load.enable_stencil_load =
                                read_but_not_cleared & PIPE_CLEAR_STENCIL;
                }
        }

        /* Tile Coordinates triggers the reload and sets where the stores
         * go. There must be one per store packet.
         */
        cl_emit(cl, TILE_COORDINATES_IMPLICIT, coords);

        /* The binner starts out writing tiles assuming that the initial mode
         * is triangles, so make sure that's the case.
         */
        cl_emit(cl, PRIMITIVE_LIST_FORMAT, fmt) {
                fmt.data_type = LIST_INDEXED;
                fmt.primitive_type = LIST_TRIANGLES;
        }

        cl_emit(cl, BRANCH_TO_IMPLICIT_TILE_LIST, branch);

        bool needs_color_clear = job->cleared & pipe_clear_color_buffers;
        bool needs_z_clear = job->cleared & PIPE_CLEAR_DEPTH;
        bool needs_s_clear = job->cleared & PIPE_CLEAR_STENCIL;
        /* Note that only the color RT being stored will be cleared by a
         * STORE_GENERAL, or all of them if the buffer is NONE.
         */
        bool msaa_color_clear = (needs_color_clear &&
                                 (job->cleared & pipe_clear_color_buffers) ==
                                 (job->resolve & pipe_clear_color_buffers));

        uint32_t stores_pending = job->resolve;

        /* Use raw stores for any MSAA surfaces.  These output UIF tiled
         * images where each 4x MSAA pixel is a 2x2 quad, and the format will
         * be that of the internal_type/internal_bpp, rather than the format
         * from GL's perspective.
         */
        for (int i = 0; i < VC5_MAX_DRAW_BUFFERS; i++) {
                uint32_t bit = PIPE_CLEAR_COLOR0 << i;
                if (!(job->resolve & bit))
                        continue;

                struct pipe_surface *psurf = job->cbufs[i];
                if (!psurf || psurf->texture->nr_samples <= 1)
                        continue;

                stores_pending &= ~bit;
                store_raw(cl, psurf, RENDER_TARGET_0 + i,
                          !stores_pending && msaa_color_clear,
                          !stores_pending && needs_z_clear,
                          !stores_pending && needs_s_clear);

                if (stores_pending)
                        cl_emit(cl, TILE_COORDINATES_IMPLICIT, coords);
        }

        if (job->resolve & PIPE_CLEAR_DEPTHSTENCIL && job->zsbuf &&
            job->zsbuf->texture->nr_samples > 1) {
                stores_pending &= ~PIPE_CLEAR_DEPTHSTENCIL;
                store_raw(cl, job->zsbuf,
                          zs_buffer_from_pipe_bits(job->resolve),
                          false,
                          !stores_pending && needs_z_clear,
                          !stores_pending && needs_s_clear);

                if (stores_pending)
                        cl_emit(cl, TILE_COORDINATES_IMPLICIT, coords);
        }

        if (stores_pending) {
                cl_emit(cl, STORE_MULTI_SAMPLE_RESOLVED_TILE_COLOR_BUFFER_EXTENDED, store) {

                        store.disable_color_buffer_write =
                                (~stores_pending >> first_color_buffer_bit) & 0xf;
                        store.enable_z_write = stores_pending & PIPE_CLEAR_DEPTH;
                        store.enable_stencil_write = stores_pending & PIPE_CLEAR_STENCIL;

                        /* Note that when set this will clear all of the color
                         * buffers.
                         */
                        store.disable_colour_buffers_clear_on_write =
                                !needs_color_clear;
                        store.disable_z_buffer_clear_on_write =
                                !needs_z_clear;
                        store.disable_stencil_buffer_clear_on_write =
                                !needs_s_clear;
                };
        } else if (needs_color_clear && !msaa_color_clear) {
                /* If we had MSAA color stores that didn't match the set of
                 * MSAA color clears, then we need to clear the color buffers
                 * now.
                 */
                cl_emit(&job->rcl, STORE_TILE_BUFFER_GENERAL, store) {
                        store.buffer_to_store = NONE;
                }
        }

        cl_emit(cl, RETURN_FROM_SUB_LIST, ret);

        cl_emit(&job->rcl, START_ADDRESS_OF_GENERIC_TILE_LIST, branch) {
                branch.start = tile_list_start;
                branch.end = cl_get_address(cl);
        }
}

#define div_round_up(a, b) (((a) + (b) - 1) / b)

void
vc5_emit_rcl(struct vc5_job *job)
{
        /* The RCL list should be empty. */
        assert(!job->rcl.bo);

        vc5_cl_ensure_space_with_branch(&job->rcl, 200 + 256 *
                                        cl_packet_length(SUPERTILE_COORDINATES));
        job->submit.rcl_start = job->rcl.bo->offset;
        vc5_job_add_bo(job, job->rcl.bo);

        int nr_cbufs = 0;
        for (int i = 0; i < VC5_MAX_DRAW_BUFFERS; i++) {
                if (job->cbufs[i])
                        nr_cbufs = i + 1;
        }

        /* Comon config must be the first TILE_RENDERING_MODE_CONFIGURATION
         * and Z_STENCIL_CLEAR_VALUES must be last.  The ones in between are
         * optional updates to the previous HW state.
         */
        cl_emit(&job->rcl, TILE_RENDERING_MODE_CONFIGURATION_COMMON_CONFIGURATION,
                config) {
                config.enable_z_store = job->resolve & PIPE_CLEAR_DEPTH;
                config.enable_stencil_store = job->resolve & PIPE_CLEAR_STENCIL;

                config.early_z_disable = !job->uses_early_z;

                config.image_width_pixels = job->draw_width;
                config.image_height_pixels = job->draw_height;

                config.number_of_render_targets_minus_1 =
                        MAX2(nr_cbufs, 1) - 1;

                config.multisample_mode_4x = job->msaa;

                config.maximum_bpp_of_all_render_targets = job->internal_bpp;
        }

        for (int i = 0; i < nr_cbufs; i++) {
                struct pipe_surface *psurf = job->cbufs[i];
                if (!psurf)
                        continue;
                struct vc5_surface *surf = vc5_surface(psurf);
                struct vc5_resource *rsc = vc5_resource(psurf->texture);

                uint32_t config_pad = 0;
                uint32_t clear_pad = 0;

                /* XXX: Set the pad for raster. */
                if (surf->tiling == VC5_TILING_UIF_NO_XOR ||
                    surf->tiling == VC5_TILING_UIF_XOR) {
                        int uif_block_height = vc5_utile_height(rsc->cpp) * 2;
                        uint32_t implicit_padded_height = (align(job->draw_height, uif_block_height) /
                                                           uif_block_height);
                        if (surf->padded_height_of_output_image_in_uif_blocks -
                            implicit_padded_height < 15) {
                                config_pad = (surf->padded_height_of_output_image_in_uif_blocks -
                                              implicit_padded_height);
                        } else {
                                config_pad = 15;
                                clear_pad = surf->padded_height_of_output_image_in_uif_blocks;
                        }
                }

                cl_emit(&job->rcl, TILE_RENDERING_MODE_CONFIGURATION_RENDER_TARGET_CONFIG, rt) {
                        rt.address = cl_address(rsc->bo, surf->offset);
                        rt.internal_type = surf->internal_type;
                        rt.output_image_format = surf->format;
                        rt.memory_format = surf->tiling;
                        rt.internal_bpp = surf->internal_bpp;
                        rt.render_target_number = i;
                        rt.pad = config_pad;

                        if (job->resolve & PIPE_CLEAR_COLOR0 << i)
                                rsc->writes++;
                }

                cl_emit(&job->rcl, TILE_RENDERING_MODE_CONFIGURATION_CLEAR_COLORS_PART1,
                        clear) {
                        clear.clear_color_low_32_bits = job->clear_color[i][0];
                        clear.clear_color_next_24_bits = job->clear_color[i][1] & 0xffffff;
                        clear.render_target_number = i;
                };

                if (surf->internal_bpp >= V3D_INTERNAL_BPP_64) {
                        cl_emit(&job->rcl, TILE_RENDERING_MODE_CONFIGURATION_CLEAR_COLORS_PART2,
                                clear) {
                                clear.clear_color_mid_low_32_bits =
                                        ((job->clear_color[i][1] >> 24) |
                                         (job->clear_color[i][2] << 8));
                                clear.clear_color_mid_high_24_bits =
                                        ((job->clear_color[i][2] >> 24) |
                                         ((job->clear_color[i][3] & 0xffff) << 8));
                                clear.render_target_number = i;
                        };
                }

                if (surf->internal_bpp >= V3D_INTERNAL_BPP_128 || clear_pad) {
                        cl_emit(&job->rcl, TILE_RENDERING_MODE_CONFIGURATION_CLEAR_COLORS_PART3,
                                clear) {
                                clear.uif_padded_height_in_uif_blocks = clear_pad;
                                clear.clear_color_high_16_bits = job->clear_color[i][3] >> 16;
                                clear.render_target_number = i;
                        };
                }
        }

        /* TODO: Don't bother emitting if we don't load/clear Z/S. */
        if (job->zsbuf) {
                struct pipe_surface *psurf = job->zsbuf;
                struct vc5_surface *surf = vc5_surface(psurf);
                struct vc5_resource *rsc = vc5_resource(psurf->texture);

                cl_emit(&job->rcl, TILE_RENDERING_MODE_CONFIGURATION_Z_STENCIL_CONFIG, zs) {
                        zs.address = cl_address(rsc->bo, surf->offset);

                        zs.internal_type = surf->internal_type;
                        zs.output_image_format = surf->format;
                        zs.padded_height_of_output_image_in_uif_blocks =
                                surf->padded_height_of_output_image_in_uif_blocks;

                        assert(surf->tiling != VC5_TILING_RASTER);
                        zs.memory_format = surf->tiling;
                }

                if (job->resolve & PIPE_CLEAR_DEPTHSTENCIL)
                        rsc->writes++;

                /* Emit the separate stencil packet if we have a resource for
                 * it.  The HW will only load/store this buffer if the
                 * Z/Stencil config doesn't have stencil in its format.
                 */
                if (rsc->separate_stencil) {
                        cl_emit(&job->rcl,
                                TILE_RENDERING_MODE_CONFIGURATION_Z_STENCIL_CONFIG,
                                zs) {
                                zs.address =
                                        cl_address(rsc->separate_stencil->bo,
                                                   surf->separate_stencil_offset);

                                zs.z_stencil_id = 1; /* Separate stencil */

                                zs.padded_height_of_output_image_in_uif_blocks =
                                        surf->separate_stencil_padded_height_of_output_image_in_uif_blocks;

                                assert(surf->tiling != VC5_TILING_RASTER);
                                zs.memory_format = surf->separate_stencil_tiling;
                        }
                }
        }

        /* Ends rendering mode config. */
        cl_emit(&job->rcl, TILE_RENDERING_MODE_CONFIGURATION_Z_STENCIL_CLEAR_VALUES,
                clear) {
                clear.z_clear_value = job->clear_z;
                clear.stencil_vg_mask_clear_value = job->clear_s;
        };

        /* Always set initial block size before the first branch, which needs
         * to match the value from binning mode config.
         */
        cl_emit(&job->rcl, TILE_LIST_INITIAL_BLOCK_SIZE, init) {
                init.use_auto_chained_tile_lists = true;
                init.size_of_first_block_in_chained_tile_lists =
                        TILE_ALLOCATION_BLOCK_SIZE_64B;
        }

        uint32_t supertile_w = 1, supertile_h = 1;

        /* If doing multicore binning, we would need to initialize each core's
         * tile list here.
         */
        cl_emit(&job->rcl, MULTICORE_RENDERING_TILE_LIST_SET_BASE, list) {
                list.address = cl_address(job->tile_alloc, 0);
        }

        cl_emit(&job->rcl, MULTICORE_RENDERING_SUPERTILE_CONFIGURATION, config) {
                uint32_t frame_w_in_supertiles, frame_h_in_supertiles;
                const uint32_t max_supertiles = 256;

                /* Size up our supertiles until we get under the limit. */
                for (;;) {
                        frame_w_in_supertiles = div_round_up(job->draw_tiles_x,
                                                             supertile_w);
                        frame_h_in_supertiles = div_round_up(job->draw_tiles_y,
                                                             supertile_h);
                        if (frame_w_in_supertiles * frame_h_in_supertiles <
                            max_supertiles) {
                                break;
                        }

                        if (supertile_w < supertile_h)
                                supertile_w++;
                        else
                                supertile_h++;
                }

                config.total_frame_width_in_tiles = job->draw_tiles_x;
                config.total_frame_height_in_tiles = job->draw_tiles_y;

                config.supertile_width_in_tiles_minus_1 = supertile_w - 1;
                config.supertile_height_in_tiles_minus_1 = supertile_h - 1;

                config.total_frame_width_in_supertiles = frame_w_in_supertiles;
                config.total_frame_height_in_supertiles = frame_h_in_supertiles;
        }

        /* Start by clearing the tile buffer. */
        cl_emit(&job->rcl, TILE_COORDINATES, coords) {
                coords.tile_column_number = 0;
                coords.tile_row_number = 0;
        }

        cl_emit(&job->rcl, STORE_TILE_BUFFER_GENERAL, store) {
                store.buffer_to_store = NONE;
        }

        cl_emit(&job->rcl, FLUSH_VCD_CACHE, flush);

        vc5_rcl_emit_generic_per_tile_list(job, nr_cbufs - 1);

        cl_emit(&job->rcl, WAIT_ON_SEMAPHORE, sem);

        /* XXX: Use Morton order */
        uint32_t supertile_w_in_pixels = job->tile_width * supertile_w;
        uint32_t supertile_h_in_pixels = job->tile_height * supertile_h;
        uint32_t min_x_supertile = job->draw_min_x / supertile_w_in_pixels;
        uint32_t min_y_supertile = job->draw_min_y / supertile_h_in_pixels;
        uint32_t max_x_supertile = (job->draw_max_x - 1) / supertile_w_in_pixels;
        uint32_t max_y_supertile = (job->draw_max_y - 1) / supertile_h_in_pixels;

        for (int y = min_y_supertile; y <= max_y_supertile; y++) {
                for (int x = min_x_supertile; x <= max_x_supertile; x++) {
                        cl_emit(&job->rcl, SUPERTILE_COORDINATES, coords) {
                                coords.column_number_in_supertiles = x;
                                coords.row_number_in_supertiles = y;
                        }
                }
        }

        cl_emit(&job->rcl, END_OF_RENDERING, end);
}

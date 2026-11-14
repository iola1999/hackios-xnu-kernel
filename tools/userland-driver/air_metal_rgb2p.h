/* Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef AIR_METAL_RGB2P_H
#define AIR_METAL_RGB2P_H
#include <stdbool.h>
#include <stddef.h>

struct rwa_texture;

/* Retains the R8/RG8 or R16/RG16 planes. Formats 540/543 use 4:2:0;
 * 544 uses 4:2:2. Sampling returns {Cr,Y,Cb,1} without a colour matrix.
 * Usage is Metal ShaderRead/RenderTarget/PixelFormatView, or Unknown (0).
 * RenderTarget requires both planes to support GPU image stores. */
struct rwa_texture *rwa_texture_combine_rgb2p(struct rwa_texture *y,
                                             struct rwa_texture *uv,
                                             unsigned format, unsigned usage,
                                             char *why, size_t why_size);

/* Queue the plane load before the first clear/draw, then the plane stores
 * after the final draw. Each begin requires a matching end. The texture
 * and its planes must remain retained until the command buffer completes.
 * Returns 0, or -1 before changing pass ownership on a validation failure. */
int rwa_texture_rgb2p_pass(struct rwa_texture *texture, bool begin,
                           char *why, size_t why_size);
#endif

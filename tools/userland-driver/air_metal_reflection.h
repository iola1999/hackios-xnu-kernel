/* Copyright © 2026 Darwin on annibale contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef AIR_METAL_REFLECTION_H
#define AIR_METAL_REFLECTION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rwa_compute;
struct rwa_reflection_type;

struct rwa_reflection_member {
   const char *name;
   unsigned offset;
   const struct rwa_reflection_type *type;
};

/* data_type uses MTLDataType values. A struct has members; an array has an
 * element, length and byte stride. Scalar and vector types have neither.
 * data_size and alignment describe the declared type in bytes.
 */
struct rwa_reflection_type {
   unsigned data_type;
   unsigned data_size;
   unsigned alignment;
   const char *name;
   unsigned member_count;
   const struct rwa_reflection_member *members;
   unsigned array_length;
   unsigned array_stride;
   const struct rwa_reflection_type *element;
};

/* type and access use MTLBindingType and MTLBindingAccess values. Builtins,
 * static threadgroup allocations and constexpr samplers are absent. An unused
 * API argument remains in this array with active=0.
 */
struct rwa_reflection_argument {
   const char *name;
   unsigned type;
   unsigned index;
   unsigned access;
   unsigned active;
   unsigned array_length;
   const struct rwa_reflection_type *buffer_type;
   unsigned constant_buffer;
   unsigned texture_type;
   unsigned texture_data_type;
   unsigned depth_texture;
};

struct rwa_compute_reflection {
   unsigned argument_count;
   const struct rwa_reflection_argument *arguments;
};

/* Returns 0 and immutable metadata owned by compute, valid until its destroy.
 * Returns -1 and a reason if the complete argument declaration or layout cannot
 * be described. out is set to NULL on failure; no partial array is returned.
 */
__attribute__((visibility("default")))
int rwa_compute_get_reflection(struct rwa_compute *compute,
                              const struct rwa_compute_reflection **out,
                              char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
#endif

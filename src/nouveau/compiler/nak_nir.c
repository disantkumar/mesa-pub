/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "nak.h"
#include "nir.h"
#include "nir_builder.h"

#define OPT(nir, pass, ...) ({                           \
   bool this_progress = false;                           \
   NIR_PASS(this_progress, nir, pass, ##__VA_ARGS__);    \
   if (this_progress)                                    \
      progress = true;                                   \
   this_progress;                                        \
})

#define OPT_V(nir, pass, ...) NIR_PASS_V(nir, pass, ##__VA_ARGS__)

static void
optimize_nir(nir_shader *nir, const struct nak_compiler *nak, bool allow_copies)
{
   bool progress;

   unsigned lower_flrp =
      (nir->options->lower_flrp16 ? 16 : 0) |
      (nir->options->lower_flrp32 ? 32 : 0) |
      (nir->options->lower_flrp64 ? 64 : 0);

   do {
      progress = false;

      /* This pass is causing problems with types used by OpenCL :
       *    https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/13955
       *
       * Running with it disabled made no difference in the resulting assembly
       * code.
       */
      if (nir->info.stage != MESA_SHADER_KERNEL)
         OPT(nir, nir_split_array_vars, nir_var_function_temp);

      OPT(nir, nir_shrink_vec_array_vars, nir_var_function_temp);
      OPT(nir, nir_opt_deref);
      if (OPT(nir, nir_opt_memcpy))
         OPT(nir, nir_split_var_copies);

      OPT(nir, nir_lower_vars_to_ssa);

      if (allow_copies) {
         /* Only run this pass in the first call to brw_nir_optimize.  Later
          * calls assume that we've lowered away any copy_deref instructions
          * and we don't want to introduce any more.
          */
         OPT(nir, nir_opt_find_array_copies);
      }
      OPT(nir, nir_opt_copy_prop_vars);
      OPT(nir, nir_opt_dead_write_vars);
      OPT(nir, nir_opt_combine_stores, nir_var_all);

      OPT(nir, nir_lower_alu_to_scalar, NULL, NULL);
      OPT(nir, nir_lower_phis_to_scalar, false);
      OPT(nir, nir_copy_prop);
      OPT(nir, nir_opt_dce);
      OPT(nir, nir_opt_cse);

      OPT(nir, nir_opt_peephole_select, 0, false, false);
      OPT(nir, nir_opt_intrinsics);
      OPT(nir, nir_opt_idiv_const, 32);
      OPT(nir, nir_opt_algebraic);
      OPT(nir, nir_lower_constant_convert_alu_types);
      OPT(nir, nir_opt_constant_folding);

      if (lower_flrp != 0) {
         if (OPT(nir, nir_lower_flrp, lower_flrp, false /* always_precise */))
            OPT(nir, nir_opt_constant_folding);
         /* Nothing should rematerialize any flrps */
         lower_flrp = 0;
      }

      OPT(nir, nir_opt_dead_cf);
      if (OPT(nir, nir_opt_trivial_continues)) {
         /* If nir_opt_trivial_continues makes progress, then we need to clean
          * things up if we want any hope of nir_opt_if or nir_opt_loop_unroll
          * to make progress.
          */
         OPT(nir, nir_copy_prop);
         OPT(nir, nir_opt_dce);
      }
      OPT(nir, nir_opt_if, nir_opt_if_optimize_phi_true_false);
      OPT(nir, nir_opt_conditional_discard);
      if (nir->options->max_unroll_iterations != 0) {
         OPT(nir, nir_opt_loop_unroll);
      }
      OPT(nir, nir_opt_remove_phis);
      OPT(nir, nir_opt_gcm, false);
      OPT(nir, nir_opt_undef);
      OPT(nir, nir_lower_pack);
   } while (progress);

   OPT(nir, nir_remove_dead_variables, nir_var_function_temp, NULL);
}

void
nak_optimize_nir(nir_shader *nir, const struct nak_compiler *nak)
{
   optimize_nir(nir, nak, false);
}

void
nak_preprocess_nir(nir_shader *nir, const struct nak_compiler *nak)
{
   UNUSED bool progress = false;

   nir_validate_ssa_dominance(nir, "before nak_preprocess_nir");

   const nir_lower_tex_options tex_options = {
      .lower_txp = ~0,
      /* TODO: More lowering */
   };
   OPT(nir, nir_lower_tex, &tex_options);

   OPT(nir, nir_lower_global_vars_to_local);

   OPT(nir, nir_split_var_copies);
   OPT(nir, nir_split_struct_vars, nir_var_function_temp);

   /* Optimize but allow copies because we haven't lowered them yet */
   optimize_nir(nir, nak, true /* allow_copies */);

   OPT(nir, nir_lower_load_const_to_scalar);
   OPT(nir, nir_lower_var_copies);
   OPT(nir, nir_lower_system_values);
   OPT(nir, nir_lower_compute_system_values, NULL);
}

static uint16_t
nak_attribute_attr_addr(gl_vert_attrib attrib)
{
   assert(attrib >= VERT_ATTRIB_GENERIC0);
   return 0x80 + (attrib - VERT_ATTRIB_GENERIC0) * 0x10;
}

static int
count_location_bytes(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false) * 16;
}

static bool
nak_nir_lower_vs_inputs(nir_shader *nir)
{
   bool progress = false;

   nir_foreach_shader_in_variable(var, nir) {
      var->data.driver_location =
         nak_attribute_attr_addr(var->data.location);
   }

   progress |= OPT(nir, nir_lower_io, nir_var_shader_in, count_location_bytes,
                        nir_lower_io_lower_64bit_to_32);

   return progress;
}

static uint16_t
nak_varying_attr_addr(gl_varying_slot slot)
{
   if (slot >= VARYING_SLOT_PATCH0) {
      return 0x020 + (slot - VARYING_SLOT_PATCH0) * 0x10;
   } else if (slot >= VARYING_SLOT_VAR0) {
      return 0x080 + (slot - VARYING_SLOT_VAR0) * 0x10;
   } else {
      switch (slot) {
      case VARYING_SLOT_TESS_LEVEL_OUTER: return 0x000;
      case VARYING_SLOT_TESS_LEVEL_INNER: return 0x010;
      case VARYING_SLOT_PRIMITIVE_ID:     return 0x060;
      case VARYING_SLOT_LAYER:            return 0x064;
      case VARYING_SLOT_VIEWPORT:         return 0x068;
      case VARYING_SLOT_PSIZ:             return 0x06c;
      case VARYING_SLOT_POS:              return 0x070;
      case VARYING_SLOT_CLIP_DIST0:       return 0x2c0;
      case VARYING_SLOT_CLIP_DIST1:       return 0x2d0;
      default: unreachable("Invalid varying slot");
      }
   }
}

static bool
nak_nir_lower_varyings(nir_shader *nir, nir_variable_mode modes)
{
   bool progress = false;

   assert(!(modes & ~(nir_var_shader_in | nir_var_shader_out)));

   nir_foreach_variable_with_modes(var, nir, modes)
      var->data.driver_location = nak_varying_attr_addr(var->data.location);

   progress |= OPT(nir, nir_lower_io, modes, count_location_bytes, 0);

   return progress;
}

static int
vec_size_4(const struct glsl_type *type, bool bindless)
{
   assert(glsl_type_is_vector_or_scalar(type));
   return 4;
}

static bool
nak_nir_lower_fs_outputs(nir_shader *nir)
{
   const uint32_t color_targets =
      (nir->info.outputs_written & BITFIELD_BIT(FRAG_RESULT_COLOR)) ?
      1 : (nir->info.outputs_written >> FRAG_RESULT_DATA0);
   const bool writes_depth =
      nir->info.outputs_written & BITFIELD_BIT(FRAG_RESULT_DEPTH);
   const bool writes_sample_mask =
      nir->info.outputs_written & BITFIELD_BIT(FRAG_RESULT_SAMPLE_MASK);

   nir->num_outputs = util_bitcount(color_targets) * 4 +
                      writes_depth + writes_sample_mask;

   nir_foreach_shader_out_variable(var, nir) {
      assert(nir->info.outputs_written & BITFIELD_BIT(var->data.location));
      switch (var->data.location) {
      case FRAG_RESULT_DEPTH:
         var->data.driver_location = util_bitcount(color_targets) * 4;
         break;
      case FRAG_RESULT_COLOR:
         var->data.driver_location = 0;
         break;
      case FRAG_RESULT_SAMPLE_MASK:
         var->data.driver_location = util_bitcount(color_targets) * 4;
         var->data.driver_location += writes_depth;
         break;
      default: {
         assert(var->data.location >= FRAG_RESULT_DATA0);
         const unsigned out = var->data.location - FRAG_RESULT_DATA0;
         var->data.driver_location =
            util_bitcount(color_targets & BITFIELD_MASK(out));
         break;
      }
      }
   }

   bool progress = nir->info.outputs_written != 0;
   progress |= OPT(nir, nir_lower_io, nir_var_shader_out, vec_size_4, 0);

   return progress;
}

static uint16_t
nak_sysval_attr_addr(gl_system_value sysval)
{
   switch (sysval) {
   case SYSTEM_VALUE_FRAG_COORD:    return 0x070;
   case SYSTEM_VALUE_POINT_COORD:   return 0x2e0;
   case SYSTEM_VALUE_TESS_COORD:    return 0x2f0;
   case SYSTEM_VALUE_INSTANCE_ID:   return 0x2f8;
   case SYSTEM_VALUE_VERTEX_ID:     return 0x2fc;
   default: unreachable("Invalid system value");
   }
}

static uint8_t
nak_sysval_sysval_idx(gl_system_value sysval)
{
   switch (sysval) {
   case SYSTEM_VALUE_SUBGROUP_INVOCATION:    return 0x00;
   case SYSTEM_VALUE_VERTICES_IN:            return 0x10;
   case SYSTEM_VALUE_INVOCATION_ID:          return 0x11;
   case SYSTEM_VALUE_HELPER_INVOCATION:      return 0x13;
   case SYSTEM_VALUE_LOCAL_INVOCATION_INDEX: return 0x20;
   case SYSTEM_VALUE_LOCAL_INVOCATION_ID:    return 0x21;
   case SYSTEM_VALUE_WORKGROUP_ID:           return 0x25;
   case SYSTEM_VALUE_SUBGROUP_EQ_MASK:       return 0x38;
   case SYSTEM_VALUE_SUBGROUP_LT_MASK:       return 0x39;
   case SYSTEM_VALUE_SUBGROUP_LE_MASK:       return 0x3a;
   case SYSTEM_VALUE_SUBGROUP_GT_MASK:       return 0x3b;
   case SYSTEM_VALUE_SUBGROUP_GE_MASK:       return 0x3c;
   default: unreachable("Invalid system value");
   }
}

static bool
nak_nir_lower_system_value_instr(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   b->cursor = nir_before_instr(instr);

   nir_def *val;
   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   switch (intrin->intrinsic) {
   case nir_intrinsic_load_sample_pos: {
      const uint32_t addr = nak_sysval_attr_addr(SYSTEM_VALUE_FRAG_COORD);
      val = nir_load_input(b, 2, 32, nir_imm_int(b, 0), .base = addr,
                           .dest_type = nir_type_float32);
      val = nir_ffract(b, val);
      break;
   }

   case nir_intrinsic_load_layer_id: {
      const uint32_t addr = nak_varying_attr_addr(VARYING_SLOT_LAYER);
      val = nir_load_input(b, intrin->def.num_components, 32,
                           nir_imm_int(b, 0), .base = addr,
                           .dest_type = nir_type_int32);
      break;
   }

   case nir_intrinsic_load_frag_coord:
   case nir_intrinsic_load_point_coord:
   case nir_intrinsic_load_tess_coord:
   case nir_intrinsic_load_instance_id:
   case nir_intrinsic_load_vertex_id: {
      const gl_system_value sysval =
         nir_system_value_from_intrinsic(intrin->intrinsic);
      const uint32_t addr = nak_sysval_attr_addr(sysval);
      val = nir_load_input(b, intrin->def.num_components, 32,
                           nir_imm_int(b, 0), .base = addr,
                           .dest_type = nir_type_int32);
      break;
   }

   case nir_intrinsic_load_subgroup_invocation:
   case nir_intrinsic_load_patch_vertices_in:
   case nir_intrinsic_load_helper_invocation:
   case nir_intrinsic_load_local_invocation_index:
   case nir_intrinsic_load_local_invocation_id:
   case nir_intrinsic_load_workgroup_id:
   case nir_intrinsic_load_workgroup_id_zero_base:
   case nir_intrinsic_load_subgroup_eq_mask:
   case nir_intrinsic_load_subgroup_lt_mask:
   case nir_intrinsic_load_subgroup_le_mask:
   case nir_intrinsic_load_subgroup_gt_mask:
   case nir_intrinsic_load_subgroup_ge_mask: {
      const gl_system_value sysval =
         intrin->intrinsic == nir_intrinsic_load_workgroup_id_zero_base ?
         SYSTEM_VALUE_WORKGROUP_ID :
         nir_system_value_from_intrinsic(intrin->intrinsic);
      const uint32_t idx = nak_sysval_sysval_idx(sysval);
      nir_def *comps[3];
      assert(intrin->def.num_components <= 3);
      for (unsigned c = 0; c < intrin->def.num_components; c++) {
         comps[c] = nir_load_sysval_nv(b, 32, .base = idx + c,
                                       .access = ACCESS_CAN_REORDER);
      }
      val = nir_vec(b, comps, intrin->def.num_components);
      break;
   }

   default:
      return false;
   }

   nir_def_rewrite_uses(&intrin->def, val);

   return true;
}

static bool
nak_nir_lower_system_values(nir_shader *nir)
{
   return nir_shader_instructions_pass(nir, nak_nir_lower_system_value_instr,
                                       nir_metadata_block_index |
                                       nir_metadata_dominance,
                                       NULL);
}

void
nak_postprocess_nir(nir_shader *nir, const struct nak_compiler *nak)
{
   UNUSED bool progress = false;

   OPT(nir, nir_lower_int64);

   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   switch (nir->info.stage) {
   case MESA_SHADER_VERTEX:
      OPT(nir, nak_nir_lower_vs_inputs);
      OPT(nir, nak_nir_lower_varyings, nir_var_shader_out);
      break;

   case MESA_SHADER_FRAGMENT:
      OPT(nir, nak_nir_lower_varyings, nir_var_shader_in);
      OPT(nir, nak_nir_lower_fs_outputs);
      break;

   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_KERNEL:
      break;

   default:
      unreachable("Unsupported shader stage");
   }

   OPT(nir, nak_nir_lower_system_values);

   nak_optimize_nir(nir, nak);
   nir_divergence_analysis(nir);

   /* Compact SSA defs because we'll use them to index arrays */
   nir_foreach_function(func, nir) {
      if (func->impl)
         nir_index_ssa_defs(func->impl);
   }

   nir_print_shader(nir, stderr);
}
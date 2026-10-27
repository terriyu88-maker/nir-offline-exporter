#include "ir.h"

#include "compiler/shader_enums.h"
#include "util/mesa-blake3.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

static const char *sizes[] = { "x??", "   ", "x2 ", "x3 ", "x4 ",
                               "x5 ", "x??", "x??", "x8 ",
                               "x??", "x??", "x??", "x??",
                               "x??", "x??", "x??", "x16" };

static unsigned
count_digits(unsigned n)
{
   return n ? (unsigned)floor(log10(n)) + 1u : 1u;
}

static const char *
comp_mask_string(unsigned num_components)
{
   return (num_components > 4) ? "abcdefghijklmnop" : "xyzw";
}

static void
print_indent(FILE *fp)
{
   fputs("    ", fp);
}

static unsigned
padding_for_no_dest(unsigned max_dest_index)
{
   return 5 + 1 + 1 + count_digits(max_dest_index) + 1 + 1 + 1;
}

static void
print_def(FILE *fp, const struct own_ir_value *v, unsigned max_dest_index)
{
   unsigned ssa_padding =
      max_dest_index ? count_digits(max_dest_index) - count_digits(v->index) : 0;
   unsigned padding = (v->bit_size <= 8) + 1 + ssa_padding;
   unsigned nc = v->num_components < ARRAY_SIZE(sizes) ? v->num_components : 0;

   fprintf(fp, "%u%s%*s%%%u", v->bit_size, sizes[nc], padding, "", v->index);
}

static const char *
mode_str(nir_variable_mode mode, bool want_local_global)
{
   switch (mode) {
   case nir_var_shader_in:
      return "shader_in";
   case nir_var_shader_out:
      return "shader_out";
   case nir_var_uniform:
      return "uniform";
   case nir_var_mem_ubo:
      return "ubo";
   case nir_var_system_value:
      return "system";
   case nir_var_mem_ssbo:
      return "ssbo";
   case nir_var_mem_shared:
      return "shared";
   case nir_var_mem_global:
      return "global";
   case nir_var_mem_push_const:
      return "push_const";
   case nir_var_mem_constant:
      return "constant";
   case nir_var_image:
      return "image";
   case nir_var_shader_temp:
      return want_local_global ? "shader_temp" : "";
   case nir_var_function_temp:
      return want_local_global ? "function_temp" : "";
   default:
      return "";
   }
}

static const char *
location_str(int location, mesa_shader_stage stage, nir_variable_mode mode,
             char *buf, size_t buf_size)
{
   switch (stage) {
   case MESA_SHADER_FRAGMENT:
      if (mode == nir_var_shader_in)
         return gl_varying_slot_name_for_stage(location, stage);
      if (mode == nir_var_shader_out)
         return gl_frag_result_name(location);
      break;
   case MESA_SHADER_VERTEX:
      if (mode == nir_var_shader_out)
         return gl_varying_slot_name_for_stage(location, stage);
      break;
   default:
      if (mode == nir_var_shader_in || mode == nir_var_shader_out)
         return gl_varying_slot_name_for_stage(location, stage);
      break;
   }

   if (location == ~0)
      return "~0";
   snprintf(buf, buf_size, "%u", (unsigned)location);
   return buf;
}

static void
print_access(FILE *fp, unsigned access, const char *separator)
{
   if (!access) {
      fputs("none", fp);
      return;
   }

   static const struct {
      enum gl_access_qualifier bit;
      const char *name;
   } modes[] = {
      { ACCESS_COHERENT, "coherent" },
      { ACCESS_RESTRICT, "restrict" },
      { ACCESS_VOLATILE, "volatile" },
      { ACCESS_NON_WRITEABLE, "readonly" },
      { ACCESS_NON_READABLE, "writeonly" },
      { ACCESS_NON_UNIFORM, "non-uniform" },
      { ACCESS_CAN_REORDER, "reorderable" },
      { ACCESS_NON_TEMPORAL, "non-temporal" },
      { ACCESS_INCLUDE_HELPERS, "include-helpers" },
      { ACCESS_IS_SWIZZLED_AMD, "is-swizzled-amd" },
      { ACCESS_USES_FORMAT_AMD, "uses-format-amd" },
      { ACCESS_FMASK_LOWERED_AMD, "fmask-lowered-amd" },
      { ACCESS_CAN_SPECULATE, "speculatable" },
      { ACCESS_CP_GE_COHERENT_AMD, "cp-ge-coherent-amd" },
      { ACCESS_IN_BOUNDS, "in-bounds" },
      { ACCESS_KEEP_SCALAR, "keep-scalar" },
      { ACCESS_SMEM_AMD, "smem-amd" },
      { ACCESS_SKIP_HELPERS, "skip-helpers" },
      { ACCESS_ATOMIC, "atomic" },
      { ACCESS_FUSED_EU_DISABLE_INTEL, "fused-eu-disable-intel" },
      { ACCESS_SPARSE, "sparse" },
      { ACCESS_ISTREAM_PAN, "istream-pan" },
      { ACCESS_ESTREAM_PAN, "estream-pan" },
   };

   bool first = true;
   for (unsigned i = 0; i < ARRAY_SIZE(modes); i++) {
      if (access & modes[i].bit) {
         fprintf(fp, "%s%s", first ? "" : separator, modes[i].name);
         first = false;
      }
   }
}

static void
print_float_u32(FILE *fp, uint32_t u, unsigned bit_size)
{
   nir_const_value v;
   memset(&v, 0, sizeof(v));
   v.u32 = u;
   double dval = nir_const_value_as_float(v, bit_size);
   if (fabs(dval) >= 1000000.0)
      fprintf(fp, "%e", dval);
   else
      fprintf(fp, "%f", dval);
}

static void
print_hex_padded_u32(FILE *fp, uint32_t u, unsigned bit_size)
{
   switch (bit_size) {
   case 64:
      fprintf(fp, "0x%016" PRIx64, (uint64_t)u);
      break;
   case 32:
      fprintf(fp, "0x%08x", u);
      break;
   case 16:
      fprintf(fp, "0x%04x", u & 0xffff);
      break;
   case 8:
      fprintf(fp, "0x%02x", u & 0xff);
      break;
   default:
      fprintf(fp, "0x%x", u);
      break;
   }
}

static void
print_load_const_values(FILE *fp, const struct own_ir_load_const *lc,
                        bool inferred_float, bool inferred_int)
{
   unsigned bit_size = lc->dest.bit_size;
   unsigned nc = lc->dest.num_components;

   if (bit_size == 1) {
      fputc('(', fp);
      for (unsigned i = 0; i < nc; i++) {
         if (i)
            fputs(", ", fp);
         fputs(lc->raw[i] ? "true" : "false", fp);
      }
      fputc(')', fp);
      return;
   }

   bool needs_float = bit_size > 8;
   bool needs_signed = false;
   bool needs_decimal = false;
   for (unsigned i = 0; i < nc; i++) {
      int32_t s = (int32_t)lc->raw[i];
      needs_signed |= s < 0;
      needs_decimal |= lc->raw[i] >= 10;
   }
   if (inferred_int && !inferred_float)
      needs_float = false;
   else if (inferred_float && !inferred_int) {
      needs_signed = false;
      needs_decimal = false;
   }

   fputc('(', fp);
   for (unsigned i = 0; i < nc; i++) {
      if (i)
         fputs(", ", fp);
      print_hex_padded_u32(fp, lc->raw[i], bit_size);
   }
   if (needs_float) {
      if (nc > 1)
         fputs(") = (", fp);
      else
         fputs(" = ", fp);
      for (unsigned i = 0; i < nc; i++) {
         if (i)
            fputs(", ", fp);
         print_float_u32(fp, lc->raw[i], bit_size);
      }
   }
   if (needs_signed) {
      if (nc > 1)
         fputs(") = (", fp);
      else
         fputs(" = ", fp);
      for (unsigned i = 0; i < nc; i++) {
         if (i)
            fputs(", ", fp);
         fprintf(fp, "%+d", (int32_t)lc->raw[i]);
      }
   }
   if (needs_decimal) {
      if (nc > 1)
         fputs(") = (", fp);
      else
         fputs(" = ", fp);
      for (unsigned i = 0; i < nc; i++) {
         if (i)
            fputs(", ", fp);
         fprintf(fp, "%u", lc->raw[i]);
      }
   }
   fputc(')', fp);
}

static void
print_const_float_src(FILE *fp, const struct own_ir_alu_src *src)
{
   unsigned nc = src->const_components ? src->const_components : src->src_components;
   unsigned bit_size = src->const_bit_size ? src->const_bit_size : 32;
   fputc('(', fp);
   for (unsigned i = 0; i < nc; i++) {
      if (i)
         fputs(", ", fp);
      print_float_u32(fp, src->const_u32[i], bit_size);
   }
   fputc(')', fp);
}

static void
print_alu_src(FILE *fp, const struct own_ir_alu *alu, unsigned src_i)
{
   const struct own_ir_alu_src *src = &alu->src[src_i];
   fprintf(fp, "%%%u", src->ssa);

   if (src->is_const) {
      fputc(' ', fp);
      nir_alu_type type = nir_alu_type_get_base_type(nir_op_infos[alu->op].input_types[src_i]);
      if (type == nir_type_float)
         print_const_float_src(fp, src);
      else
         print_const_float_src(fp, src);
   }

   bool print_swizzle = false;
   unsigned used_channels = 0;
   unsigned dest_nc = alu->dest.num_components;
   unsigned live = src->src_components ? src->src_components : dest_nc;
   unsigned out_size = nir_op_infos[alu->op].output_size;
   unsigned used = out_size ? dest_nc : dest_nc;

   for (unsigned i = 0; i < used && i < NIR_MAX_VEC_COMPONENTS; i++) {
      used_channels++;
      if (src->swizzle[i] != i)
         print_swizzle = true;
   }
    if (print_swizzle || used_channels != live) {
       fputc('.', fp);
       const char *xyzw = comp_mask_string(live);
       for (unsigned i = 0; i < used && i < NIR_MAX_VEC_COMPONENTS; i++)
          fputc(xyzw[src->swizzle[i]], fp);
    }
}

static void
print_hex_terse_u32(FILE *fp, uint32_t u, unsigned bit_size)
{
   switch (bit_size) {
   case 64:
      fprintf(fp, "0x%" PRIx64, (uint64_t)u);
      break;
   default:
      fprintf(fp, "0x%x", u);
      break;
   }
}

static void
print_const_typed(FILE *fp, const uint32_t *raw, unsigned nc, unsigned bit_size,
                  nir_alu_type type)
{
   type = nir_alu_type_get_base_type(type);
   fputc('(', fp);
   if (bit_size == 1 || type == nir_type_bool) {
      for (unsigned i = 0; i < nc; i++) {
         if (i)
            fputs(", ", fp);
         fputs(raw[i] ? "true" : "false", fp);
      }
      fputc(')', fp);
      return;
   }
   for (unsigned i = 0; i < nc; i++) {
      if (i)
         fputs(", ", fp);
      if (type == nir_type_float)
         print_float_u32(fp, raw[i], bit_size);
      else
         print_hex_terse_u32(fp, raw[i], bit_size);
   }
   fputc(')', fp);
}

static void
print_alu_type_name(FILE *fp, nir_alu_type type)
{
   unsigned size = nir_alu_type_get_type_size(type);
   const char *name;
   switch (nir_alu_type_get_base_type(type)) {
   case nir_type_int:
      name = "int";
      break;
   case nir_type_uint:
      name = "uint";
      break;
   case nir_type_bool:
      name = "bool";
      break;
   case nir_type_float:
      name = "float";
      break;
   default:
      name = "invalid";
      break;
   }
   if (size)
      fprintf(fp, "%s%u", name, size);
   else
      fputs(name, fp);
}

static const char *
tex_op_name(nir_texop op)
{
   switch (op) {
   case nir_texop_tex:
      return "tex";
   case nir_texop_txb:
      return "txb";
   case nir_texop_txl:
      return "txl";
   case nir_texop_txd:
      return "txd";
   case nir_texop_txf:
      return "txf";
   case nir_texop_txf_ms:
      return "txf_ms";
   case nir_texop_txf_ms_fb:
      return "txf_ms_fb";
   case nir_texop_txf_ms_mcs_intel:
      return "txf_ms_mcs_intel";
   case nir_texop_txs:
      return "txs";
   case nir_texop_lod:
      return "lod";
   case nir_texop_tg4:
      return "tg4";
   case nir_texop_query_levels:
      return "query_levels";
   case nir_texop_texture_samples:
      return "texture_samples";
   case nir_texop_samples_identical:
      return "samples_identical";
   case nir_texop_tex_prefetch:
      return "tex (pre-dispatchable)";
   case nir_texop_lod_bias:
      return "lod_bias";
   case nir_texop_fragment_fetch_amd:
      return "fragment_fetch_amd";
   case nir_texop_fragment_mask_fetch_amd:
      return "fragment_mask_fetch_amd";
   case nir_texop_descriptor_amd:
      return "descriptor_amd";
   case nir_texop_sampler_descriptor_amd:
      return "sampler_descriptor_amd";
   case nir_texop_image_min_lod_agx:
      return "image_min_lod_agx";
   case nir_texop_has_custom_border_color_agx:
      return "has_custom_border_color_agx";
   case nir_texop_custom_border_color_agx:
      return "custom_border_color_agx";
   case nir_texop_hdr_dim_nv:
      return "hdr_dim_nv";
   case nir_texop_tex_type_nv:
      return "tex_type_nv";
   case nir_texop_sample_pos_nv:
      return "sample_pos_nv";
   case nir_texop_gradient_pan:
      return "gradient_pan";
   case nir_texop_sample_weighted_qcom:
      return "sample_weighted_qcom";
   case nir_texop_box_filter_qcom:
      return "box_filter_qcom";
   case nir_texop_block_match_sad_qcom:
      return "block_match_sad_qcom";
   case nir_texop_block_match_ssd_qcom:
      return "block_match_ssd_qcom";
   case nir_texop_resinfo_intel:
      return "resinfo_intel";
   case nir_texop_sparse_residency_intel:
      return "sparse_residency_intel";
   case nir_texop_sparse_residency_txf_intel:
      return "sparse_residency_txf_intel";
   default:
      return "tex";
   }
}

static const char *
tex_src_type_name(nir_tex_src_type t)
{
   switch (t) {
   case nir_tex_src_backend1:
      return "(backend1)";
   case nir_tex_src_backend2:
      return "(backend2)";
   case nir_tex_src_coord:
      return "(coord)";
   case nir_tex_src_ref_coord:
      return "(ref_coord)";
   case nir_tex_src_projector:
      return "(projector)";
   case nir_tex_src_comparator:
      return "(comparator)";
   case nir_tex_src_offset:
      return "(offset)";
   case nir_tex_src_bias:
      return "(bias)";
   case nir_tex_src_lod:
      return "(lod)";
   case nir_tex_src_min_lod:
      return "(min_lod)";
   case nir_tex_src_max_lod_kk:
      return "(max_lod_kk)";
   case nir_tex_src_ms_index:
      return "(ms_index)";
   case nir_tex_src_ms_mcs_intel:
      return "(ms_mcs_intel)";
   case nir_tex_src_lod_bias_min_agx:
      return "(lod_bias_min_agx)";
   case nir_tex_src_ddx:
      return "(ddx)";
   case nir_tex_src_ddy:
      return "(ddy)";
   case nir_tex_src_sampler_deref_intrinsic:
      return "(sampler_deref_intrinsic)";
   case nir_tex_src_texture_deref_intrinsic:
      return "(texture_deref_intrinsic)";
   case nir_tex_src_texture_deref:
      return "(texture_deref)";
   case nir_tex_src_texture_2_deref:
      return "(texture_2_deref)";
   case nir_tex_src_sampler_deref:
      return "(sampler_deref)";
   case nir_tex_src_sampler_2_deref:
      return "(sampler_2_deref)";
   case nir_tex_src_texture_offset:
      return "(texture_offset)";
   case nir_tex_src_sampler_offset:
      return "(sampler_offset)";
   case nir_tex_src_texture_handle:
      return "(texture_handle)";
   case nir_tex_src_texture_2_handle:
      return "(texture_2_handle)";
   case nir_tex_src_sampler_handle:
      return "(sampler_handle)";
   case nir_tex_src_sampler_2_handle:
      return "(ref_sampler_handle)";
   case nir_tex_src_block_size:
      return "(block_size)";
   case nir_tex_src_box_size:
      return "(box_size)";
   case nir_tex_src_texture_heap_offset:
      return "(texture_heap_offset)";
   case nir_tex_src_sampler_heap_offset:
      return "(sampler_heap_offset)";
   case nir_tex_src_plane:
      return "(plane)";
   default:
      return "(unknown)";
   }
}

static const char *
sampler_dim_str(enum glsl_sampler_dim dim)
{
   static const char *names[] = {
      [GLSL_SAMPLER_DIM_1D] = "1D",
      [GLSL_SAMPLER_DIM_2D] = "2D",
      [GLSL_SAMPLER_DIM_3D] = "3D",
      [GLSL_SAMPLER_DIM_CUBE] = "Cube",
      [GLSL_SAMPLER_DIM_RECT] = "Rect",
      [GLSL_SAMPLER_DIM_BUF] = "Buf",
      [GLSL_SAMPLER_DIM_EXTERNAL] = "External",
      [GLSL_SAMPLER_DIM_MS] = "2D-MSAA",
      [GLSL_SAMPLER_DIM_SUBPASS] = "Subpass",
      [GLSL_SAMPLER_DIM_SUBPASS_MS] = "Subpass-MSAA",
   };
   if (dim < ARRAY_SIZE(names) && names[dim])
      return names[dim];
   return "2D";
}

static void
print_tex(FILE *fp, const struct own_ir_tex *tex, unsigned max_dest)
{
   print_def(fp, &tex->dest, max_dest);
   fputs(" = (", fp);
   print_alu_type_name(fp, tex->dest_type);
   fprintf(fp, ")%s ", tex_op_name(tex->op));

   bool has_texture_deref = false, has_sampler_deref = false;
   for (unsigned i = 0; i < tex->num_srcs; i++) {
      if (i > 0)
         fputs(", ", fp);
      const struct own_ir_tex_src *src = &tex->src[i];
      fprintf(fp, "%%%u", src->ssa);
      if (src->is_const) {
         fputc(' ', fp);
         print_const_typed(fp, src->const_u32, src->const_components,
                           src->const_bit_size, src->src_alu_type);
      }
      fputc(' ', fp);
      fputs(tex_src_type_name(src->src_type), fp);
      switch (src->src_type) {
      case nir_tex_src_sampler_deref_intrinsic:
      case nir_tex_src_texture_deref_intrinsic:
      case nir_tex_src_texture_deref:
         has_texture_deref = true;
         break;
      case nir_tex_src_sampler_deref:
         has_sampler_deref = true;
         break;
      default:
         break;
      }
   }

   if (tex->is_gather_implicit_lod)
      fputs(", implicit lod", fp);
   if (tex->op == nir_texop_tg4)
      fprintf(fp, ", %u (gather_component)", tex->component);
   if (tex->has_explicit_tg4_offsets) {
      fprintf(fp, ", { (%i, %i)", tex->tg4_offsets[0][0], tex->tg4_offsets[0][1]);
      for (unsigned i = 1; i < 4; i++)
         fprintf(fp, ", (%i, %i)", tex->tg4_offsets[i][0], tex->tg4_offsets[i][1]);
      fputs(" } (offsets)", fp);
   }
   if (tex->op != nir_texop_txf_ms_fb && !has_texture_deref)
      fprintf(fp, ", %u (texture)", tex->texture_index);
   if (tex->need_sampler && !has_sampler_deref)
      fprintf(fp, ", %u (sampler)", tex->sampler_index);
   if (tex->texture_non_uniform)
      fputs(", texture non-uniform", fp);
   if (tex->sampler_non_uniform)
      fputs(", sampler non-uniform", fp);
   if (tex->is_sparse)
      fputs(", sparse", fp);
   if (tex->skip_helpers)
      fputs(", skip_helpers", fp);
   if (tex->backend_flags)
      fprintf(fp, ", backend_flags=0x%X", tex->backend_flags);
   fprintf(fp, ", %s", sampler_dim_str(tex->sampler_dim));
}

static void
print_var(FILE *fp, const struct own_ir_module *m, const struct own_ir_var *var)
{
   fprintf(fp, "decl_var %s %s ",
           mode_str(var->mode, false),
           glsl_interp_mode_name(var->interpolation));
    print_access(fp, var->access, " ");
    fputc(' ', fp);
   fprintf(fp, "%s %s", glsl_get_type_name(var->type), var->name);

   if (var->mode & (nir_var_shader_in | nir_var_shader_out |
                    nir_var_uniform | nir_var_system_value |
                    nir_var_mem_ubo | nir_var_mem_ssbo | nir_var_image)) {
      char buf[16];
      const char *loc = location_str(var->location, m->stage, var->mode,
                                     buf, sizeof(buf));
      unsigned num_components =
         glsl_get_components(glsl_without_array(var->type));
      char components_local[18] = { '.' };
      const char *components = "";
      if ((var->mode == nir_var_shader_in || var->mode == nir_var_shader_out) &&
          num_components < 16 && num_components != 0) {
         const char *xyzw = comp_mask_string(num_components);
         for (unsigned i = 0; i < num_components; i++)
            components_local[i + 1] = xyzw[i + var->location_frac];
         components = components_local;
      }
      fprintf(fp, " (%s%s, %u, %u)", loc, components,
              var->driver_location, var->binding);
   }
   fputc('\n', fp);
}

static bool
block_has_dest(const struct own_ir_block *block)
{
   util_dynarray_foreach(&block->instrs, struct own_ir_instr *, pip) {
      struct own_ir_instr *instr = *pip;
      switch (instr->kind) {
      case OWN_IR_INSTR_ALU:
      case OWN_IR_INSTR_DEREF:
      case OWN_IR_INSTR_LOAD_CONST:
      case OWN_IR_INSTR_TEX:
      case OWN_IR_INSTR_PHI:
      case OWN_IR_INSTR_UNDEF:
         return true;
      case OWN_IR_INSTR_INTRINSIC:
         if (instr->intrinsic.has_dest)
            return true;
         break;
      default:
         break;
      }
   }
   return false;
}

static void
print_block_preds(FILE *fp, const struct own_ir_block *block)
{
   unsigned n = util_dynarray_num_elements(&block->preds, unsigned);
   unsigned *preds = util_dynarray_element(&block->preds, unsigned, 0);
   for (unsigned i = 0; i < n; i++)
      fprintf(fp, " b%u", preds[i]);
}

static void
print_block_succs(FILE *fp, const struct own_ir_block *block)
{
   unsigned n = util_dynarray_num_elements(&block->succs, unsigned);
   unsigned *succs = util_dynarray_element(&block->succs, unsigned, 0);
   for (unsigned i = 0; i < n; i++)
      fprintf(fp, " b%u", succs[i]);
}

static void
print_instr(FILE *fp, const struct own_ir_instr *instr, unsigned max_dest,
            unsigned no_dest_pad, const BITSET_WORD *float_types,
            const BITSET_WORD *int_types)
{
   print_indent(fp);

   switch (instr->kind) {
   case OWN_IR_INSTR_DEREF: {
      print_def(fp, &instr->deref.dest, max_dest);
      if (instr->deref.deref_type == nir_deref_type_var) {
         fprintf(fp, " = deref_var &%s (", instr->deref.var_name);
         unsigned modes = instr->deref.modes;
         bool first = true;
         while (modes) {
            int bit = u_bit_scan(&modes);
            fprintf(fp, "%s%s", first ? "" : "|",
                    mode_str((nir_variable_mode)(1u << bit), true));
            first = false;
         }
         fprintf(fp, " %s)", glsl_get_type_name(instr->deref.type));
      } else {
         fprintf(fp, " = deref_%u", (unsigned)instr->deref.deref_type);
      }
      break;
   }

   case OWN_IR_INSTR_INTRINSIC: {
      if (instr->intrinsic.has_dest) {
         print_def(fp, &instr->intrinsic.dest, max_dest);
         fputs(" = ", fp);
      } else if (no_dest_pad) {
         fprintf(fp, "%*s", no_dest_pad, "");
      }
      fprintf(fp, "@%s", instr->intrinsic.name);
      if (instr->intrinsic.num_srcs) {
         fputs(" (", fp);
         for (unsigned i = 0; i < instr->intrinsic.num_srcs; i++) {
            if (i)
               fputs(", ", fp);
            fprintf(fp, "%%%u", instr->intrinsic.src_ssa[i]);
         }
         fputc(')', fp);
      }
      {
         const nir_intrinsic_info *info =
            &nir_intrinsic_infos[instr->intrinsic.op];
         bool any = false;
         for (unsigned i = 0; i < info->num_indices; i++) {
            unsigned idx = info->indices[i];
            if (idx != NIR_INTRINSIC_WRITE_MASK && idx != NIR_INTRINSIC_ACCESS)
               continue;
            fputs(any ? ", " : " (", fp);
            any = true;
            if (idx == NIR_INTRINSIC_WRITE_MASK) {
               unsigned wrmask = instr->intrinsic.write_mask;
               fputs("wrmask=", fp);
               unsigned nc = instr->intrinsic.num_components;
               const char *xyzw = comp_mask_string(nc);
               for (unsigned c = 0; c < nc; c++)
                  if ((wrmask >> c) & 1)
                     fputc(xyzw[c], fp);
            } else {
                fputs("access=", fp);
                print_access(fp, instr->intrinsic.access, "|");
            }
         }
         if (any)
            fputc(')', fp);
      }
      break;
   }

   case OWN_IR_INSTR_LOAD_CONST: {
      print_def(fp, &instr->load_const.dest, max_dest);
      fputs(" = load_const ", fp);
      unsigned index = instr->load_const.dest.index;
      bool inf = float_types && BITSET_TEST(float_types, index);
      bool ini = int_types && BITSET_TEST(int_types, index);
      print_load_const_values(fp, &instr->load_const, inf, ini);
      break;
   }

   case OWN_IR_INSTR_ALU: {
      print_def(fp, &instr->alu.dest, max_dest);
      fprintf(fp, " = %s ", nir_op_infos[instr->alu.op].name);
      for (unsigned i = 0; i < instr->alu.num_srcs; i++) {
         if (i)
            fputs(", ", fp);
         print_alu_src(fp, &instr->alu, i);
      }
      break;
   }

   case OWN_IR_INSTR_JUMP:
      switch (instr->jump.type) {
      case nir_jump_return:
         fputs("return", fp);
         break;
      case nir_jump_halt:
         fputs("halt", fp);
         break;
      case nir_jump_break:
         fputs("break", fp);
         break;
      case nir_jump_continue:
         fputs("continue", fp);
         break;
      default:
         fprintf(fp, "jump %u", (unsigned)instr->jump.type);
         break;
      }
      break;

    case OWN_IR_INSTR_TEX:
       print_tex(fp, &instr->tex, max_dest);
       break;

   case OWN_IR_INSTR_PHI:
      print_def(fp, &instr->phi.dest, max_dest);
      fputs(" = phi /* own_ir unimplemented phi */", fp);
      break;

   case OWN_IR_INSTR_UNDEF:
      print_def(fp, &instr->undef.dest, max_dest);
      fputs(" = undefined", fp);
      break;

   case OWN_IR_INSTR_UNIMPLEMENTED:
      fprintf(fp, "/* own_ir unimplemented nir_instr_type=%u */",
              (unsigned)instr->unimplemented.type);
      break;
   }

   fputc('\n', fp);
}

static void
print_function(FILE *fp, const struct own_ir_function *fn,
               nir_shader *nir)
{
   fprintf(fp, "decl_function %s ()", fn->name);
   if (fn->is_entrypoint)
      fputs(" (entrypoint)", fp);
   fputc('\n', fp);

   if (util_dynarray_num_elements(&fn->blocks, struct own_ir_block *) == 0)
      return;

   fprintf(fp, "\nimpl %s {\n", fn->name);

   BITSET_WORD *float_types = NULL;
   BITSET_WORD *int_types = NULL;
   nir_function_impl *impl = NULL;
   nir_foreach_function(nfn, nir) {
      if (nfn->name && fn->name && !strcmp(nfn->name, fn->name) && nfn->impl) {
         impl = nfn->impl;
         break;
      }
   }
   if (impl) {
      float_types = BITSET_CALLOC(impl->ssa_alloc);
      int_types = BITSET_CALLOC(impl->ssa_alloc);
      nir_gather_types(impl, float_types, int_types);
   }

   unsigned max_dest = fn->ssa_alloc;
   util_dynarray_foreach(&fn->blocks, struct own_ir_block *, pblock) {
      struct own_ir_block *block = *pblock;
      unsigned no_dest_pad = block_has_dest(block) ? padding_for_no_dest(max_dest) : 0;

      if (block->is_end_block) {
         print_indent(fp);
         fprintf(fp, "block b%u:\n", block->index);
         continue;
      }

      print_indent(fp);
      fprintf(fp, "block b%u:", block->index);

      unsigned ninstr =
         util_dynarray_num_elements(&block->instrs, struct own_ir_instr *);
      if (ninstr == 0) {
         fputs("  // preds:", fp);
         print_block_preds(fp, block);
         fputs(", succs:", fp);
         print_block_succs(fp, block);
         fputc('\n', fp);
         continue;
      }

      unsigned block_length = 7 + count_digits(block->index) + 1;
      unsigned pred_padding =
         block_length < no_dest_pad ? no_dest_pad - block_length : 0;
      fprintf(fp, "%*s// preds:", pred_padding, "");
      print_block_preds(fp, block);
      fputc('\n', fp);

      util_dynarray_foreach(&block->instrs, struct own_ir_instr *, pip)
         print_instr(fp, *pip, max_dest, no_dest_pad, float_types, int_types);

      print_indent(fp);
      fprintf(fp, "%*s// succs:", no_dest_pad, "");
      print_block_succs(fp, block);
      fputc('\n', fp);
   }

   fputs("}\n\n", fp);
   free(float_types);
   free(int_types);
}

static bool
blake3_nonzero(const uint8_t *h)
{
   static const uint8_t z[32] = { 0 };
   return memcmp(h, z, 32) != 0;
}

void
own_ir_print_shader(struct nir_shader *nir, FILE *fp)
{
   struct own_ir_module *m = own_ir_walk(nir);

   fprintf(fp, "shader: %s\n", mesa_shader_stage_name(m->stage));
   if (blake3_nonzero(m->source_blake3)) {
      fputs("source_blake3: {", fp);
      _mesa_blake3_print(fp, m->source_blake3);
      fputs("}\n", fp);
   }
   if (m->name)
      fprintf(fp, "name: %s\n", m->name);
   fprintf(fp, "max_subgroup_size: %u\n", m->max_subgroup_size);
   fprintf(fp, "min_subgroup_size: %u\n", m->min_subgroup_size);
   if (m->origin_upper_left)
      fputs("origin_upper_left: true\n", fp);
   if (m->num_inputs)
      fprintf(fp, "inputs: %u\n", m->num_inputs);
   if (m->num_outputs)
      fprintf(fp, "outputs: %u\n", m->num_outputs);
   if (m->num_uniforms)
      fprintf(fp, "uniforms: %u\n", m->num_uniforms);
   if (m->scratch_size)
      fprintf(fp, "scratch: %u\n", m->scratch_size);

   util_dynarray_foreach(&m->variables, struct own_ir_var *, pv)
      print_var(fp, m, *pv);

   util_dynarray_foreach(&m->functions, struct own_ir_function *, pfn)
      print_function(fp, *pfn, nir);
}

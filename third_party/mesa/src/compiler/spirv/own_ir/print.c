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
print_indent(FILE *fp, unsigned tabs)
{
   for (unsigned i = 0; i < tabs; i++)
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
       if (mode == nir_var_shader_in)
          return gl_vert_attrib_name(location);
       if (mode == nir_var_shader_out)
          return gl_varying_slot_name_for_stage(location, stage);
       break;
   default:
      if (mode == nir_var_shader_in || mode == nir_var_shader_out)
         return gl_varying_slot_name_for_stage(location, stage);
      break;
   }

   if (mode == nir_var_system_value)
      return gl_system_value_name(location);

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

static void print_const_typed(FILE *fp, const uint32_t *raw, unsigned nc,
                              unsigned bit_size, nir_alu_type type);

static void
print_alu_src(FILE *fp, const struct own_ir_alu *alu, unsigned src_i)
{
   const struct own_ir_alu_src *src = &alu->src[src_i];
   fprintf(fp, "%%%u", src->ssa);

    if (src->is_const) {
       fputc(' ', fp);
       nir_alu_type type = nir_alu_type_get_base_type(nir_op_infos[alu->op].input_types[src_i]);
       if (type == nir_type_invalid)
          type = nir_type_uint;
       if (type == nir_type_float)
          print_const_float_src(fp, src);
       else
          print_const_typed(fp, src->const_u32, src->const_components,
                            src->const_bit_size, type);
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

static const char *
deref_kind_name(nir_deref_type t)
{
   switch (t) {
   case nir_deref_type_var:
      return "deref_var";
   case nir_deref_type_array:
   case nir_deref_type_array_wildcard:
      return "deref_array";
   case nir_deref_type_struct:
      return "deref_struct";
   case nir_deref_type_cast:
      return "deref_cast";
   case nir_deref_type_ptr_as_array:
      return "deref_ptr_as_array";
   default:
      return "deref";
   }
}

static void
print_deref_modes_and_type(FILE *fp, const struct own_ir_deref *d)
{
   fputc('(', fp);
   unsigned modes = d->modes;
   while (modes) {
      int bit = u_bit_scan(&modes);
      fprintf(fp, "%s%s", mode_str((nir_variable_mode)(1u << bit), true),
              modes ? "|" : "");
   }
    fprintf(fp, " %s)", d->type_name ? d->type_name : "");
}

static void
print_deref(FILE *fp, const struct own_ir_deref *d, unsigned max_dest)
{
   print_def(fp, &d->dest, max_dest);
   fprintf(fp, " = %s ", deref_kind_name(d->deref_type));

   if (d->deref_type != nir_deref_type_cast)
      fputc('&', fp);

   switch (d->deref_type) {
   case nir_deref_type_var:
      fputs(d->var_name, fp);
      break;
   case nir_deref_type_cast:
       fprintf(fp, "(%s *)%%%u", d->type_name ? d->type_name : "", d->parent_ssa);
      break;
   case nir_deref_type_struct:
      fprintf(fp, "%%%u->%s", d->parent_ssa, d->struct_field);
      break;
   case nir_deref_type_array:
   case nir_deref_type_ptr_as_array:
      fputc('(', fp);
      fputc('*', fp);
      fprintf(fp, "%%%u)", d->parent_ssa);
      if (d->arr_index_is_const)
         fprintf(fp, "[%" PRId64 "]", d->arr_index_const);
      else
         fprintf(fp, "[%%%u]", d->arr_index_ssa);
      break;
   case nir_deref_type_array_wildcard:
      fprintf(fp, "%%%u[*]", d->parent_ssa);
      break;
   default:
      fprintf(fp, "%%%u", d->parent_ssa);
      break;
   }

   fputc(' ', fp);
   print_deref_modes_and_type(fp, d);

   if (d->deref_type == nir_deref_type_cast) {
      fprintf(fp, "  (ptr_stride=%u, align_mul=%u, align_offset=%u)",
              d->ptr_stride, d->align_mul, d->align_offset);
   }
   if ((d->deref_type == nir_deref_type_array ||
        d->deref_type == nir_deref_type_ptr_as_array) &&
       d->arr_in_bounds) {
      fputs("  (in bounds)", fp);
   }
   if (d->deref_type != nir_deref_type_var &&
       d->deref_type != nir_deref_type_cast &&
       d->chain_comment && d->chain_comment[0]) {
      fprintf(fp, "  // &%s", d->chain_comment);
   }
}

static const char *
descriptor_type_name(nir_descriptor_type type)
{
   switch (type) {
   case nir_descriptor_type_uniform_buffer:
      return "UBO";
   case nir_descriptor_type_storage_buffer:
      return "SSBO";
   case nir_descriptor_type_acceleration_structure:
      return "accel-struct";
   default:
      return "unknown";
   }
}

static const char *
atomic_op_name(nir_atomic_op op)
{
   switch (op) {
   case nir_atomic_op_iadd: return "iadd";
   case nir_atomic_op_isub: return "isub";
   case nir_atomic_op_imin: return "imin";
   case nir_atomic_op_umin: return "umin";
   case nir_atomic_op_imax: return "imax";
   case nir_atomic_op_umax: return "umax";
   case nir_atomic_op_iand: return "iand";
   case nir_atomic_op_ior: return "ior";
   case nir_atomic_op_ixor: return "ixor";
   case nir_atomic_op_xchg: return "xchg";
   case nir_atomic_op_fadd: return "fadd";
   case nir_atomic_op_fmin: return "fmin";
   case nir_atomic_op_fmax: return "fmax";
   case nir_atomic_op_cmpxchg: return "cmpxchg";
   case nir_atomic_op_fcmpxchg: return "fcmpxchg";
   case nir_atomic_op_inc_wrap: return "inc_wrap";
   case nir_atomic_op_dec_wrap: return "dec_wrap";
   case nir_atomic_op_ordered_add_gfx12_amd: return "ordered_add";
   default: return "unknown";
   }
}

static const char *
resource_type_name(nir_resource_type type)
{
   switch (type) {
   case nir_resource_type_sampler:
      return "sampler";
   case nir_resource_type_sampled_image:
      return "texture";
   case nir_resource_type_read_only_image:
      return "RO-image";
   case nir_resource_type_read_write_image:
      return "RW-image";
   case nir_resource_type_combined_sampled_image:
      return "texture+sampler";
   case nir_resource_type_uniform_buffer:
      return "UBO";
   case nir_resource_type_read_only_storage_buffer:
      return "RO-SSBO";
   case nir_resource_type_read_write_storage_buffer:
      return "RW-SSBO";
   case nir_resource_type_acceleration_structure:
      return "accel-struct";
   default:
      return "unknown";
   }
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
    fprintf(fp, "%s %s", var->type_name ? var->type_name : "", var->name);

     if (m && (var->mode & (nir_var_shader_in | nir_var_shader_out |
                     nir_var_uniform | nir_var_system_value |
                     nir_var_mem_ubo | nir_var_mem_ssbo | nir_var_image))) {
        char buf[16];
        const char *loc = location_str(var->location, m->stage, var->mode,
                                       buf, sizeof(buf));
        unsigned num_components = var->type_components;
       char components_local[18] = { '.' };
       const char *components = "";
       if ((var->mode == nir_var_shader_in || var->mode == nir_var_shader_out) &&
           num_components < 16 && num_components != 0) {
          const char *xyzw = comp_mask_string(num_components);
          for (unsigned i = 0; i < num_components; i++)
             components_local[i + 1] = xyzw[i + var->location_frac];
          components = components_local;
       }
       if (var->mode & nir_var_system_value)
          fprintf(fp, " (%s%s)", loc, components);
       else
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
print_fp_math_ctrl(FILE *fp, uint32_t fp_math_ctrl)
{
   if (fp_math_ctrl & nir_fp_exact) {
      if ((fp_math_ctrl & nir_fp_exact) == nir_fp_exact)
         fputs("exact", fp);
      else if (fp_math_ctrl & nir_fp_no_contract)
         fputs("no-contract", fp);
      else if (fp_math_ctrl & nir_fp_no_reassoc)
         fputs("no-reassoc", fp);
      else if (fp_math_ctrl & nir_fp_no_transform)
         fputs("no-transform", fp);
      if (fp_math_ctrl & ~nir_fp_exact)
         fputs(", ", fp);
   }
   if (fp_math_ctrl & nir_fp_preserve_sz_inf_nan) {
      fputs("preserve:", fp);
      static const struct {
         nir_fp_math_control bit;
         const char *name;
      } preserve_bits[] = {
         { nir_fp_preserve_signed_zero, "sz" },
         { nir_fp_preserve_inf, "inf" },
         { nir_fp_preserve_nan, "nan" },
      };
      bool first = true;
      for (unsigned i = 0; i < ARRAY_SIZE(preserve_bits); i++) {
         if (fp_math_ctrl & preserve_bits[i].bit) {
            if (!first)
               fputc(',', fp);
            first = false;
            fputs(preserve_bits[i].name, fp);
         }
      }
   }
}

static void
print_ssa_src(FILE *fp, unsigned ssa, bool is_const, const uint32_t *const_u32,
              uint8_t const_components, uint8_t const_bit_size,
              const BITSET_WORD *float_types, const BITSET_WORD *int_types)
{
   fprintf(fp, "%%%u", ssa);
   if (!is_const)
      return;
   nir_alu_type type = nir_type_invalid;
   if (int_types) {
      bool inferred_int = BITSET_TEST(int_types, ssa);
      bool inferred_float = float_types && BITSET_TEST(float_types, ssa);
      if (inferred_float && !inferred_int)
         type = nir_type_float;
   }
   if (type == nir_type_invalid)
      type = nir_type_uint;
   fputc(' ', fp);
   print_const_typed(fp, const_u32, const_components, const_bit_size, type);
}

static void
print_instr(FILE *fp, const struct own_ir_instr *instr, unsigned tabs,
            unsigned max_dest, unsigned no_dest_pad,
            const BITSET_WORD *float_types, const BITSET_WORD *int_types)
{
   print_indent(fp, tabs);

   switch (instr->kind) {
    case OWN_IR_INSTR_DEREF:
       print_deref(fp, &instr->deref, max_dest);
       break;

    case OWN_IR_INSTR_INTRINSIC: {
       const struct own_ir_intrinsic *intr = &instr->intrinsic;
       if (intr->has_dest) {
          print_def(fp, &intr->dest, max_dest);
          fputs(" = ", fp);
       } else if (no_dest_pad) {
          fprintf(fp, "%*s", no_dest_pad, "");
       }
       fprintf(fp, "@%s", intr->name);
       if (intr->num_srcs) {
          fputs(" (", fp);
          for (unsigned i = 0; i < intr->num_srcs; i++) {
             if (i)
                fputs(", ", fp);
             const struct own_ir_intrinsic_src *src = &intr->src[i];
             fprintf(fp, "%%%u", src->ssa);
             if (src->is_const) {
                fputc(' ', fp);
                print_const_typed(fp, src->const_u32, src->const_components,
                                  src->const_bit_size, src->alu_type);
             }
          }
          fputc(')', fp);
       }
       {
          const nir_intrinsic_info *info = &nir_intrinsic_infos[intr->op];
          for (unsigned i = 0; i < info->num_indices; i++) {
             unsigned idx = info->indices[i];
             fputs(i == 0 ? " (" : ", ", fp);
             switch (idx) {
             case NIR_INTRINSIC_WRITE_MASK: {
                unsigned wrmask = intr->write_mask;
                fputs("wrmask=", fp);
                unsigned nc = intr->num_components;
                const char *xyzw = comp_mask_string(nc);
                for (unsigned c = 0; c < nc; c++)
                   if ((wrmask >> c) & 1)
                      fputc(xyzw[c], fp);
                break;
             }
             case NIR_INTRINSIC_ACCESS:
                fputs("access=", fp);
                print_access(fp, intr->access, "|");
                break;
             case NIR_INTRINSIC_DESC_TYPE:
                fprintf(fp, "desc_type=%s",
                        descriptor_type_name((nir_descriptor_type)
                           intr->const_index[info->index_map[idx] - 1]));
                break;
              case NIR_INTRINSIC_RESOURCE_TYPE:
                 fprintf(fp, "resource_type=%s",
                         resource_type_name((nir_resource_type)
                            intr->const_index[info->index_map[idx] - 1]));
                 break;
              case NIR_INTRINSIC_ATOMIC_OP:
                 fprintf(fp, "atomic_op=%s",
                         atomic_op_name((nir_atomic_op)
                            intr->const_index[info->index_map[idx] - 1]));
                 break;
              default: {
                unsigned off = info->index_map[idx] - 1;
                fprintf(fp, "%s=%d", nir_intrinsic_index_names[idx],
                        intr->const_index[off]);
                break;
             }
             }
          }
          if (info->num_indices)
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
       fprintf(fp, " = %s", nir_op_infos[instr->alu.op].name);
       if (instr->alu.no_signed_wrap)
          fputs(".nsw", fp);
       if (instr->alu.no_unsigned_wrap)
          fputs(".nuw", fp);
       fputc(' ', fp);
       for (unsigned i = 0; i < instr->alu.num_srcs; i++) {
          if (i)
             fputs(", ", fp);
          print_alu_src(fp, &instr->alu, i);
       }
       if (instr->alu.fp_math_ctrl) {
          fputs(" // ", fp);
          print_fp_math_ctrl(fp, instr->alu.fp_math_ctrl);
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

   case OWN_IR_INSTR_PHI: {
      print_def(fp, &instr->phi.dest, max_dest);
      fputs(" = phi ", fp);
      unsigned n = util_dynarray_num_elements(&instr->phi.srcs, struct own_ir_phi_src);
      struct own_ir_phi_src *srcs =
         util_dynarray_element(&instr->phi.srcs, struct own_ir_phi_src, 0);
      for (unsigned i = 0; i < n; i++) {
         if (i)
            fputs(", ", fp);
         fprintf(fp, "b%u: ", srcs[i].pred);
         print_ssa_src(fp, srcs[i].ssa, srcs[i].is_const, srcs[i].const_u32,
                       srcs[i].const_components, srcs[i].const_bit_size,
                       float_types, int_types);
      }
      break;
   }

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
print_block(FILE *fp, const struct own_ir_block *block, unsigned tabs,
            unsigned max_dest, const BITSET_WORD *float_types,
            const BITSET_WORD *int_types)
{
   unsigned no_dest_pad = block_has_dest(block) ? padding_for_no_dest(max_dest) : 0;

   print_indent(fp, tabs);
   fprintf(fp, "block b%u:", block->index);

   unsigned ninstr =
      util_dynarray_num_elements(&block->instrs, struct own_ir_instr *);
   if (ninstr == 0) {
      fputs("  // preds:", fp);
      print_block_preds(fp, block);
      fputs(", succs:", fp);
      print_block_succs(fp, block);
      fputc('\n', fp);
      return;
   }

   unsigned block_length = 7 + count_digits(block->index) + 1;
   unsigned pred_padding =
      block_length < no_dest_pad ? no_dest_pad - block_length : 0;
   fprintf(fp, "%*s// preds:", pred_padding, "");
   print_block_preds(fp, block);
   fputc('\n', fp);

   util_dynarray_foreach(&block->instrs, struct own_ir_instr *, pip)
      print_instr(fp, *pip, tabs, max_dest, no_dest_pad, float_types, int_types);

   print_indent(fp, tabs);
   fprintf(fp, "%*s// succs:", no_dest_pad, "");
   print_block_succs(fp, block);
   fputc('\n', fp);
}

static void print_cf_list(FILE *fp, const struct util_dynarray *list,
                          unsigned tabs, unsigned max_dest,
                          const BITSET_WORD *float_types,
                          const BITSET_WORD *int_types);

static void
print_if(FILE *fp, const struct own_ir_if *nif, unsigned tabs,
         unsigned max_dest, const BITSET_WORD *float_types,
         const BITSET_WORD *int_types)
{
   print_indent(fp, tabs);
   fputs("if ", fp);
   print_ssa_src(fp, nif->cond_ssa, nif->cond_is_const, nif->const_u32,
                 nif->const_components, nif->const_bit_size,
                 float_types, int_types);
   switch (nif->control) {
   case nir_selection_control_flatten:
      fputs("  // flatten", fp);
      break;
   case nir_selection_control_dont_flatten:
      fputs("  // don't flatten", fp);
      break;
   case nir_selection_control_divergent_always_taken:
      fputs("  // divergent always taken", fp);
      break;
   default:
      break;
   }
   fputs(" {\n", fp);
   print_cf_list(fp, &nif->then_list, tabs + 1, max_dest, float_types, int_types);
   print_indent(fp, tabs);
   fputs("} else {\n", fp);
   print_cf_list(fp, &nif->else_list, tabs + 1, max_dest, float_types, int_types);
   print_indent(fp, tabs);
   fputs("}\n", fp);
}

static void
print_loop(FILE *fp, const struct own_ir_loop *loop, unsigned tabs,
           unsigned max_dest, const BITSET_WORD *float_types,
           const BITSET_WORD *int_types)
{
   print_indent(fp, tabs);
   fputs("loop", fp);
   switch (loop->control) {
   case nir_loop_control_unroll:
      fputs("  // unroll", fp);
      break;
   case nir_loop_control_dont_unroll:
      fputs("  // don't unroll", fp);
      break;
   default:
      break;
   }
   fputs(" {\n", fp);
   print_cf_list(fp, &loop->body, tabs + 1, max_dest, float_types, int_types);
   print_indent(fp, tabs);
   if (loop->has_continue) {
      fputs("} continue {\n", fp);
      print_cf_list(fp, &loop->continue_list, tabs + 1, max_dest,
                    float_types, int_types);
      print_indent(fp, tabs);
   }
   fputs("}\n", fp);
}

static void
print_cf_list(FILE *fp, const struct util_dynarray *list, unsigned tabs,
              unsigned max_dest, const BITSET_WORD *float_types,
              const BITSET_WORD *int_types)
{
   util_dynarray_foreach(list, struct own_ir_cf_node *, pnode) {
      struct own_ir_cf_node *node = *pnode;
      switch (node->kind) {
      case OWN_IR_CF_BLOCK:
         print_block(fp, node->block, tabs, max_dest, float_types, int_types);
         break;
      case OWN_IR_CF_IF:
         print_if(fp, node->nif, tabs, max_dest, float_types, int_types);
         break;
      case OWN_IR_CF_LOOP:
         print_loop(fp, node->loop, tabs, max_dest, float_types, int_types);
         break;
      }
   }
}

static void
print_function(FILE *fp, const struct own_ir_function *fn)
{
   fprintf(fp, "decl_function %s ()", fn->name);
   if (fn->is_entrypoint)
      fputs(" (entrypoint)", fp);
   fputc('\n', fp);

   if (util_dynarray_num_elements(&fn->body, struct own_ir_cf_node *) == 0 &&
       !fn->end_block)
      return;

   fprintf(fp, "\nimpl %s {\n", fn->name);

   const BITSET_WORD *float_types = fn->float_types;
   const BITSET_WORD *int_types = fn->int_types;

   util_dynarray_foreach(&fn->locals, struct own_ir_var *, pv) {
      print_indent(fp, 1);
      print_var(fp, NULL, *pv);
   }

   unsigned max_dest = fn->ssa_alloc;
   print_cf_list(fp, &fn->body, 1, max_dest, float_types, int_types);

   if (fn->end_block) {
      print_indent(fp, 1);
      fprintf(fp, "block b%u:\n", fn->end_block->index);
   }

   fputs("}\n\n", fp);
}

static bool
blake3_nonzero(const uint8_t *h)
{
   static const uint8_t z[32] = { 0 };
   return memcmp(h, z, 32) != 0;
}

void
own_ir_print_module(const struct own_ir_module *m, FILE *fp)
{
   fprintf(fp, "shader: %s\n", mesa_shader_stage_name(m->stage));
   if (blake3_nonzero(m->source_blake3)) {
      fputs("source_blake3: {", fp);
      _mesa_blake3_print(fp, m->source_blake3);
      fputs("}\n", fp);
   }
     if (m->name)
        fprintf(fp, "name: %s\n", m->name);
     if (mesa_shader_stage_uses_workgroup(m->stage)) {
        fprintf(fp, "workgroup_size: %u, %u, %u%s\n",
                m->workgroup_size[0], m->workgroup_size[1], m->workgroup_size[2],
                m->workgroup_size_variable ? " (variable)" : "");
     }
     if (m->num_textures)
       fprintf(fp, "num_textures: %u\n", m->num_textures);
    if (m->num_ubos)
       fprintf(fp, "num_ubos: %u\n", m->num_ubos);
    if (m->num_abos)
       fprintf(fp, "num_abos: %u\n", m->num_abos);
    if (m->num_ssbos)
       fprintf(fp, "num_ssbos: %u\n", m->num_ssbos);
    if (m->num_images)
       fprintf(fp, "num_images: %u\n", m->num_images);
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
       print_function(fp, *pfn);
}

void
own_ir_print_shader(struct nir_shader *nir, FILE *fp)
{
   struct own_ir_module *m = own_ir_walk(nir);
   own_ir_print_module(m, fp);
}

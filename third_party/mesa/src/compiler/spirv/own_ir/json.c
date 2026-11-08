#include "ir.h"

#include "compiler/shader_enums.h"
#include "util/parson.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

struct jw {
   FILE *fp;
   unsigned indent;
   bool need_comma;
};

static void
jw_indent(struct jw *j)
{
   for (unsigned i = 0; i < j->indent; i++)
      fputs("  ", j->fp);
}

static void
jw_sep(struct jw *j)
{
   if (j->need_comma)
      fputs(",\n", j->fp);
   else
      fputc('\n', j->fp);
   jw_indent(j);
}

static void
jw_begin_obj(struct jw *j)
{
   fputc('{', j->fp);
   j->indent++;
   j->need_comma = false;
}

static void
jw_end_obj(struct jw *j)
{
   j->indent--;
   if (j->need_comma) {
      fputc('\n', j->fp);
      jw_indent(j);
   }
   fputc('}', j->fp);
   j->need_comma = true;
}

static void
jw_begin_arr(struct jw *j)
{
   fputc('[', j->fp);
   j->indent++;
   j->need_comma = false;
}

static void
jw_end_arr(struct jw *j)
{
   j->indent--;
   if (j->need_comma) {
      fputc('\n', j->fp);
      jw_indent(j);
   }
   fputc(']', j->fp);
   j->need_comma = true;
}

static void
jw_key(struct jw *j, const char *key)
{
   jw_sep(j);
   fprintf(j->fp, "\"%s\": ", key);
   j->need_comma = false;
}

static void
jw_raw_str(struct jw *j, const char *s)
{
   fputc('"', j->fp);
   if (s) {
      for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
         switch (*p) {
         case '"':
            fputs("\\\"", j->fp);
            break;
         case '\\':
            fputs("\\\\", j->fp);
            break;
         case '\n':
            fputs("\\n", j->fp);
            break;
         case '\r':
            fputs("\\r", j->fp);
            break;
         case '\t':
            fputs("\\t", j->fp);
            break;
         default:
            if (*p < 0x20)
               fprintf(j->fp, "\\u%04x", *p);
            else
               fputc(*p, j->fp);
            break;
         }
      }
   }
   fputc('"', j->fp);
   j->need_comma = true;
}

static void
jw_str(struct jw *j, const char *s)
{
   jw_raw_str(j, s ? s : "");
}

static void
jw_null(struct jw *j)
{
   fputs("null", j->fp);
   j->need_comma = true;
}

static void
jw_bool(struct jw *j, bool v)
{
   fputs(v ? "true" : "false", j->fp);
   j->need_comma = true;
}

static void
jw_u(struct jw *j, uint64_t v)
{
   fprintf(j->fp, "%" PRIu64, v);
   j->need_comma = true;
}

static void
jw_i(struct jw *j, int64_t v)
{
   fprintf(j->fp, "%" PRId64, v);
   j->need_comma = true;
}

static void
jw_key_str(struct jw *j, const char *k, const char *s)
{
   jw_key(j, k);
   jw_str(j, s);
}

static void
jw_key_bool(struct jw *j, const char *k, bool v)
{
   jw_key(j, k);
   jw_bool(j, v);
}

static void
jw_key_u(struct jw *j, const char *k, uint64_t v)
{
   jw_key(j, k);
   jw_u(j, v);
}

static void
jw_key_i(struct jw *j, const char *k, int64_t v)
{
   jw_key(j, k);
   jw_i(j, v);
}

static const char *
var_mode_name(nir_variable_mode mode)
{
   switch (mode) {
   case nir_var_system_value: return "nir_var_system_value";
   case nir_var_uniform: return "nir_var_uniform";
   case nir_var_shader_in: return "nir_var_shader_in";
   case nir_var_shader_out: return "nir_var_shader_out";
   case nir_var_image: return "nir_var_image";
   case nir_var_shader_call_data: return "nir_var_shader_call_data";
   case nir_var_ray_hit_attrib: return "nir_var_ray_hit_attrib";
   case nir_var_mem_ubo: return "nir_var_mem_ubo";
   case nir_var_mem_push_const: return "nir_var_mem_push_const";
   case nir_var_mem_ssbo: return "nir_var_mem_ssbo";
   case nir_var_mem_constant: return "nir_var_mem_constant";
   case nir_var_mem_task_payload: return "nir_var_mem_task_payload";
   case nir_var_mem_node_payload: return "nir_var_mem_node_payload";
   case nir_var_mem_node_payload_in: return "nir_var_mem_node_payload_in";
   case nir_var_mem_pixel_local_in: return "nir_var_mem_pixel_local_in";
   case nir_var_mem_pixel_local_out: return "nir_var_mem_pixel_local_out";
   case nir_var_mem_pixel_local_inout: return "nir_var_mem_pixel_local_inout";
   case nir_var_function_in: return "nir_var_function_in";
   case nir_var_function_out: return "nir_var_function_out";
   case nir_var_function_inout: return "nir_var_function_inout";
   case nir_var_resource_heap: return "nir_var_resource_heap";
   case nir_var_sampler_heap: return "nir_var_sampler_heap";
   case nir_var_shader_temp: return "nir_var_shader_temp";
   case nir_var_function_temp: return "nir_var_function_temp";
   case nir_var_mem_shared: return "nir_var_mem_shared";
   case nir_var_mem_global: return "nir_var_mem_global";
   default: return NULL;
   }
}

static const char *
alu_type_name(nir_alu_type t)
{
   switch (t) {
   case nir_type_invalid: return "nir_type_invalid";
   case nir_type_int: return "nir_type_int";
   case nir_type_uint: return "nir_type_uint";
   case nir_type_bool: return "nir_type_bool";
   case nir_type_float: return "nir_type_float";
   case nir_type_bool1: return "nir_type_bool1";
   case nir_type_bool32: return "nir_type_bool32";
   case nir_type_int1: return "nir_type_int1";
   case nir_type_int8: return "nir_type_int8";
   case nir_type_int16: return "nir_type_int16";
   case nir_type_int32: return "nir_type_int32";
   case nir_type_int64: return "nir_type_int64";
   case nir_type_uint1: return "nir_type_uint1";
   case nir_type_uint8: return "nir_type_uint8";
   case nir_type_uint16: return "nir_type_uint16";
   case nir_type_uint32: return "nir_type_uint32";
   case nir_type_uint64: return "nir_type_uint64";
   case nir_type_float16: return "nir_type_float16";
   case nir_type_float32: return "nir_type_float32";
   case nir_type_float64: return "nir_type_float64";
   default: return "nir_type_invalid";
   }
}

static const char *
deref_type_name(nir_deref_type t)
{
   switch (t) {
   case nir_deref_type_var: return "nir_deref_type_var";
   case nir_deref_type_array: return "nir_deref_type_array";
   case nir_deref_type_array_wildcard: return "nir_deref_type_array_wildcard";
   case nir_deref_type_ptr_as_array: return "nir_deref_type_ptr_as_array";
   case nir_deref_type_struct: return "nir_deref_type_struct";
   case nir_deref_type_cast: return "nir_deref_type_cast";
   default: return "nir_deref_type_var";
   }
}

static const char *
jump_type_name(nir_jump_type t)
{
   switch (t) {
   case nir_jump_return: return "nir_jump_return";
   case nir_jump_halt: return "nir_jump_halt";
   case nir_jump_abort: return "nir_jump_abort";
   case nir_jump_break: return "nir_jump_break";
   case nir_jump_continue: return "nir_jump_continue";
   case nir_jump_goto: return "nir_jump_goto";
   case nir_jump_goto_if: return "nir_jump_goto_if";
   default: return "nir_jump_return";
   }
}

static const char *
texop_name(nir_texop op)
{
   switch (op) {
   case nir_texop_tex: return "nir_texop_tex";
   case nir_texop_txb: return "nir_texop_txb";
   case nir_texop_txl: return "nir_texop_txl";
   case nir_texop_txd: return "nir_texop_txd";
   case nir_texop_txf: return "nir_texop_txf";
   case nir_texop_txf_ms: return "nir_texop_txf_ms";
   case nir_texop_txf_ms_fb: return "nir_texop_txf_ms_fb";
   case nir_texop_txf_ms_mcs_intel: return "nir_texop_txf_ms_mcs_intel";
   case nir_texop_txs: return "nir_texop_txs";
   case nir_texop_lod: return "nir_texop_lod";
   case nir_texop_tg4: return "nir_texop_tg4";
   case nir_texop_query_levels: return "nir_texop_query_levels";
   case nir_texop_texture_samples: return "nir_texop_texture_samples";
   case nir_texop_samples_identical: return "nir_texop_samples_identical";
   case nir_texop_tex_prefetch: return "nir_texop_tex_prefetch";
   case nir_texop_lod_bias: return "nir_texop_lod_bias";
   case nir_texop_fragment_fetch_amd: return "nir_texop_fragment_fetch_amd";
   case nir_texop_fragment_mask_fetch_amd: return "nir_texop_fragment_mask_fetch_amd";
   case nir_texop_descriptor_amd: return "nir_texop_descriptor_amd";
   case nir_texop_sampler_descriptor_amd: return "nir_texop_sampler_descriptor_amd";
   case nir_texop_image_min_lod_agx: return "nir_texop_image_min_lod_agx";
   case nir_texop_has_custom_border_color_agx: return "nir_texop_has_custom_border_color_agx";
   case nir_texop_custom_border_color_agx: return "nir_texop_custom_border_color_agx";
   case nir_texop_hdr_dim_nv: return "nir_texop_hdr_dim_nv";
   case nir_texop_tex_type_nv: return "nir_texop_tex_type_nv";
   case nir_texop_sample_pos_nv: return "nir_texop_sample_pos_nv";
   case nir_texop_gradient_pan: return "nir_texop_gradient_pan";
   case nir_texop_sample_weighted_qcom: return "nir_texop_sample_weighted_qcom";
   case nir_texop_box_filter_qcom: return "nir_texop_box_filter_qcom";
   case nir_texop_block_match_sad_qcom: return "nir_texop_block_match_sad_qcom";
   case nir_texop_block_match_ssd_qcom: return "nir_texop_block_match_ssd_qcom";
   case nir_texop_resinfo_intel: return "nir_texop_resinfo_intel";
   case nir_texop_sparse_residency_intel: return "nir_texop_sparse_residency_intel";
   case nir_texop_sparse_residency_txf_intel: return "nir_texop_sparse_residency_txf_intel";
   default: return "nir_texop_tex";
   }
}

static const char *
tex_src_type_enum(nir_tex_src_type t)
{
   switch (t) {
   case nir_tex_src_coord: return "nir_tex_src_coord";
   case nir_tex_src_projector: return "nir_tex_src_projector";
   case nir_tex_src_comparator: return "nir_tex_src_comparator";
   case nir_tex_src_offset: return "nir_tex_src_offset";
   case nir_tex_src_bias: return "nir_tex_src_bias";
   case nir_tex_src_lod: return "nir_tex_src_lod";
   case nir_tex_src_min_lod: return "nir_tex_src_min_lod";
   case nir_tex_src_max_lod_kk: return "nir_tex_src_max_lod_kk";
   case nir_tex_src_lod_bias_min_agx: return "nir_tex_src_lod_bias_min_agx";
   case nir_tex_src_ms_index: return "nir_tex_src_ms_index";
   case nir_tex_src_ms_mcs_intel: return "nir_tex_src_ms_mcs_intel";
   case nir_tex_src_ddx: return "nir_tex_src_ddx";
   case nir_tex_src_ddy: return "nir_tex_src_ddy";
   case nir_tex_src_texture_deref: return "nir_tex_src_texture_deref";
   case nir_tex_src_sampler_deref: return "nir_tex_src_sampler_deref";
   case nir_tex_src_texture_offset: return "nir_tex_src_texture_offset";
   case nir_tex_src_sampler_offset: return "nir_tex_src_sampler_offset";
   case nir_tex_src_texture_handle: return "nir_tex_src_texture_handle";
   case nir_tex_src_sampler_handle: return "nir_tex_src_sampler_handle";
   case nir_tex_src_texture_heap_offset: return "nir_tex_src_texture_heap_offset";
   case nir_tex_src_sampler_heap_offset: return "nir_tex_src_sampler_heap_offset";
   case nir_tex_src_sampler_deref_intrinsic: return "nir_tex_src_sampler_deref_intrinsic";
   case nir_tex_src_texture_deref_intrinsic: return "nir_tex_src_texture_deref_intrinsic";
   case nir_tex_src_plane: return "nir_tex_src_plane";
   case nir_tex_src_backend1: return "nir_tex_src_backend1";
   case nir_tex_src_backend2: return "nir_tex_src_backend2";
   case nir_tex_src_ref_coord: return "nir_tex_src_ref_coord";
   case nir_tex_src_texture_2_deref: return "nir_tex_src_texture_2_deref";
   case nir_tex_src_sampler_2_deref: return "nir_tex_src_sampler_2_deref";
   case nir_tex_src_texture_2_handle: return "nir_tex_src_texture_2_handle";
   case nir_tex_src_sampler_2_handle: return "nir_tex_src_sampler_2_handle";
   case nir_tex_src_block_size: return "nir_tex_src_block_size";
   case nir_tex_src_box_size: return "nir_tex_src_box_size";
   default: return "nir_tex_src_coord";
   }
}

static const char *
sampler_dim_enum(enum glsl_sampler_dim dim)
{
   switch (dim) {
   case GLSL_SAMPLER_DIM_1D: return "GLSL_SAMPLER_DIM_1D";
   case GLSL_SAMPLER_DIM_2D: return "GLSL_SAMPLER_DIM_2D";
   case GLSL_SAMPLER_DIM_3D: return "GLSL_SAMPLER_DIM_3D";
   case GLSL_SAMPLER_DIM_CUBE: return "GLSL_SAMPLER_DIM_CUBE";
   case GLSL_SAMPLER_DIM_RECT: return "GLSL_SAMPLER_DIM_RECT";
   case GLSL_SAMPLER_DIM_BUF: return "GLSL_SAMPLER_DIM_BUF";
   case GLSL_SAMPLER_DIM_EXTERNAL: return "GLSL_SAMPLER_DIM_EXTERNAL";
   case GLSL_SAMPLER_DIM_MS: return "GLSL_SAMPLER_DIM_MS";
   case GLSL_SAMPLER_DIM_SUBPASS: return "GLSL_SAMPLER_DIM_SUBPASS";
   case GLSL_SAMPLER_DIM_SUBPASS_MS: return "GLSL_SAMPLER_DIM_SUBPASS_MS";
   default: return "GLSL_SAMPLER_DIM_2D";
   }
}

static const char *
sel_control_name(nir_selection_control c)
{
   switch (c) {
   case nir_selection_control_flatten: return "nir_selection_control_flatten";
   case nir_selection_control_dont_flatten: return "nir_selection_control_dont_flatten";
   case nir_selection_control_divergent_always_taken:
      return "nir_selection_control_divergent_always_taken";
   default: return "nir_selection_control_none";
   }
}

static const char *
loop_control_name(nir_loop_control c)
{
   switch (c) {
   case nir_loop_control_unroll: return "nir_loop_control_unroll";
   case nir_loop_control_dont_unroll: return "nir_loop_control_dont_unroll";
   default: return "nir_loop_control_none";
   }
}

static const char *
instr_type_name(nir_instr_type t)
{
   switch (t) {
   case nir_instr_type_alu: return "nir_instr_type_alu";
   case nir_instr_type_deref: return "nir_instr_type_deref";
   case nir_instr_type_call: return "nir_instr_type_call";
   case nir_instr_type_tex: return "nir_instr_type_tex";
   case nir_instr_type_intrinsic: return "nir_instr_type_intrinsic";
   case nir_instr_type_load_const: return "nir_instr_type_load_const";
   case nir_instr_type_jump: return "nir_instr_type_jump";
   case nir_instr_type_undef: return "nir_instr_type_undef";
   case nir_instr_type_phi: return "nir_instr_type_phi";
   case nir_instr_type_cmat_call: return "nir_instr_type_cmat_call";
   default: return "nir_instr_type_alu";
   }
}

static void
jw_block_label(struct jw *j, unsigned index)
{
   char buf[32];
   snprintf(buf, sizeof(buf), "b%u", index);
   jw_str(j, buf);
}

static void
jw_access_arr(struct jw *j, unsigned access)
{
   static const struct {
      enum gl_access_qualifier bit;
      const char *name;
   } bits[] = {
      { ACCESS_COHERENT, "ACCESS_COHERENT" },
      { ACCESS_RESTRICT, "ACCESS_RESTRICT" },
      { ACCESS_VOLATILE, "ACCESS_VOLATILE" },
      { ACCESS_NON_WRITEABLE, "ACCESS_NON_WRITEABLE" },
      { ACCESS_NON_READABLE, "ACCESS_NON_READABLE" },
      { ACCESS_NON_UNIFORM, "ACCESS_NON_UNIFORM" },
      { ACCESS_CAN_REORDER, "ACCESS_CAN_REORDER" },
      { ACCESS_NON_TEMPORAL, "ACCESS_NON_TEMPORAL" },
      { ACCESS_INCLUDE_HELPERS, "ACCESS_INCLUDE_HELPERS" },
      { ACCESS_IS_SWIZZLED_AMD, "ACCESS_IS_SWIZZLED_AMD" },
      { ACCESS_USES_FORMAT_AMD, "ACCESS_USES_FORMAT_AMD" },
      { ACCESS_FMASK_LOWERED_AMD, "ACCESS_FMASK_LOWERED_AMD" },
      { ACCESS_CAN_SPECULATE, "ACCESS_CAN_SPECULATE" },
      { ACCESS_CP_GE_COHERENT_AMD, "ACCESS_CP_GE_COHERENT_AMD" },
      { ACCESS_IN_BOUNDS, "ACCESS_IN_BOUNDS" },
      { ACCESS_KEEP_SCALAR, "ACCESS_KEEP_SCALAR" },
      { ACCESS_SMEM_AMD, "ACCESS_SMEM_AMD" },
      { ACCESS_SKIP_HELPERS, "ACCESS_SKIP_HELPERS" },
      { ACCESS_ATOMIC, "ACCESS_ATOMIC" },
      { ACCESS_FUSED_EU_DISABLE_INTEL, "ACCESS_FUSED_EU_DISABLE_INTEL" },
      { ACCESS_SPARSE, "ACCESS_SPARSE" },
      { ACCESS_ISTREAM_PAN, "ACCESS_ISTREAM_PAN" },
      { ACCESS_ESTREAM_PAN, "ACCESS_ESTREAM_PAN" },
   };

   jw_begin_arr(j);
   for (unsigned i = 0; i < ARRAY_SIZE(bits); i++) {
      if (access & bits[i].bit) {
         jw_sep(j);
         jw_str(j, bits[i].name);
      }
   }
   jw_end_arr(j);
}

static void
jw_mode_bits(struct jw *j, nir_variable_mode modes)
{
   jw_begin_arr(j);
   for (unsigned i = 0; i < nir_num_variable_modes; i++) {
      nir_variable_mode bit = (nir_variable_mode)(1u << i);
      if (modes & bit) {
         const char *n = var_mode_name(bit);
         if (n) {
            jw_sep(j);
            jw_str(j, n);
         }
      }
   }
   jw_end_arr(j);
}

static void
jw_u32_vec(struct jw *j, const uint32_t *v, unsigned n)
{
   jw_begin_arr(j);
   for (unsigned i = 0; i < n; i++) {
      jw_sep(j);
      jw_u(j, v[i]);
   }
   jw_end_arr(j);
}

static void
jw_value(struct jw *j, const struct own_ir_value *v)
{
   jw_begin_obj(j);
   jw_key_u(j, "index", v->index);
   jw_key_u(j, "bit_size", v->bit_size);
   jw_key_u(j, "num_components", v->num_components);
   jw_end_obj(j);
}

static void
jw_src_const_fields(struct jw *j, unsigned ssa, bool is_const,
                    uint8_t const_components, uint8_t const_bit_size,
                    const uint32_t *const_u32)
{
   jw_key_u(j, "ssa", ssa);
   jw_key_bool(j, "is_const", is_const);
   if (is_const) {
      jw_key_u(j, "const_components", const_components);
      jw_key_u(j, "const_bit_size", const_bit_size);
      jw_key(j, "const_u32");
      jw_u32_vec(j, const_u32, const_components);
   }
}

static void
jw_var(struct jw *j, const struct own_ir_var *var)
{
   jw_begin_obj(j);
   {
      const char *mode = var_mode_name(var->mode);
      jw_key_str(j, "mode", mode ? mode : "");
   }
    jw_key_str(j, "interpolation", glsl_interp_mode_name(var->interpolation));
    jw_key(j, "type");
    if (var->type_name && var->type_name[0])
       jw_str(j, var->type_name);
    else if (var->type)
       jw_str(j, glsl_get_type_name(var->type));
    else
       jw_null(j);
    jw_key_u(j, "type_components", var->type_components);
    jw_key_str(j, "name", var->name);
   jw_key_i(j, "location", var->location);
   jw_key_u(j, "location_frac", var->location_frac);
   jw_key_u(j, "component_mask", var->component_mask);
   jw_key_u(j, "driver_location", var->driver_location);
    jw_key_u(j, "binding", var->binding);
    jw_key(j, "access");
    jw_access_arr(j, var->access);
    if (var->image_format_name && var->image_format_name[0])
       jw_key_str(j, "image_format", var->image_format_name);
    jw_end_obj(j);
}

static void
jw_alu(struct jw *j, const struct own_ir_alu *alu)
{
   jw_begin_obj(j);
   jw_key(j, "dest");
   jw_value(j, &alu->dest);
   jw_key_str(j, "op", nir_op_infos[alu->op].name);
   jw_key_bool(j, "no_signed_wrap", alu->no_signed_wrap);
   jw_key_bool(j, "no_unsigned_wrap", alu->no_unsigned_wrap);
   jw_key_u(j, "fp_math_ctrl", alu->fp_math_ctrl);
   jw_key(j, "srcs");
   jw_begin_arr(j);
   for (unsigned i = 0; i < alu->num_srcs; i++) {
      const struct own_ir_alu_src *s = &alu->src[i];
      jw_sep(j);
      jw_begin_obj(j);
      jw_src_const_fields(j, s->ssa, s->is_const, s->const_components,
                          s->const_bit_size, s->const_u32);
      jw_key_u(j, "src_components", s->src_components);
      jw_key(j, "swizzle");
      {
         unsigned n = alu->dest.num_components;
         jw_begin_arr(j);
         for (unsigned c = 0; c < n; c++) {
            jw_sep(j);
            jw_u(j, s->swizzle[c]);
         }
         jw_end_arr(j);
      }
      jw_end_obj(j);
   }
   jw_end_arr(j);
   jw_end_obj(j);
}

static void
jw_deref(struct jw *j, const struct own_ir_deref *d)
{
   jw_begin_obj(j);
   jw_key(j, "dest");
   jw_value(j, &d->dest);
   jw_key_str(j, "deref_type", deref_type_name(d->deref_type));
   jw_key(j, "modes");
   jw_mode_bits(j, d->modes);
    jw_key(j, "type");
    if (d->type_name && d->type_name[0])
       jw_str(j, d->type_name);
    else if (d->type)
       jw_str(j, glsl_get_type_name(d->type));
    else
       jw_null(j);
   if (d->deref_type == nir_deref_type_var) {
      jw_key_str(j, "var_name", d->var_name);
   } else {
      jw_key_u(j, "parent_ssa", d->parent_ssa);
   }
   if (d->deref_type == nir_deref_type_struct) {
      jw_key_u(j, "struct_index", d->struct_index);
      jw_key_str(j, "struct_field", d->struct_field);
   }
   if (d->deref_type == nir_deref_type_array ||
       d->deref_type == nir_deref_type_array_wildcard ||
       d->deref_type == nir_deref_type_ptr_as_array) {
      jw_key_bool(j, "arr_in_bounds", d->arr_in_bounds);
      jw_key_bool(j, "arr_index_is_const", d->arr_index_is_const);
      if (d->arr_index_is_const)
         jw_key_i(j, "arr_index_const", d->arr_index_const);
      else if (d->deref_type != nir_deref_type_array_wildcard)
         jw_key_u(j, "arr_index_ssa", d->arr_index_ssa);
   }
   if (d->deref_type == nir_deref_type_cast) {
      jw_key_u(j, "ptr_stride", d->ptr_stride);
      jw_key_u(j, "align_mul", d->align_mul);
      jw_key_u(j, "align_offset", d->align_offset);
   }
   if (d->chain_comment && d->chain_comment[0])
      jw_key_str(j, "chain_comment", d->chain_comment);
   jw_end_obj(j);
}

static void
jw_intrinsic(struct jw *j, const struct own_ir_intrinsic *intr)
{
   jw_begin_obj(j);
   jw_key_bool(j, "has_dest", intr->has_dest);
    if (intr->has_dest) {
       jw_key(j, "dest");
       jw_value(j, &intr->dest);
       jw_key_str(j, "dest_alu_type", alu_type_name(intr->dest_alu_type));
    }
    jw_key_str(j, "op", nir_intrinsic_infos[intr->op].name);
   jw_key_u(j, "num_components", intr->num_components);
   jw_key_u(j, "write_mask", intr->write_mask);
   jw_key(j, "access");
   jw_access_arr(j, intr->access);
   jw_key(j, "const_index");
   jw_begin_arr(j);
   {
      const nir_intrinsic_info *info = &nir_intrinsic_infos[intr->op];
      unsigned n = info->num_index_slots;
      if (n > NIR_INTRINSIC_MAX_CONST_INDEX)
         n = NIR_INTRINSIC_MAX_CONST_INDEX;
      for (unsigned i = 0; i < n; i++) {
         jw_sep(j);
         jw_i(j, intr->const_index[i]);
      }
   }
   jw_end_arr(j);
   jw_key(j, "srcs");
   jw_begin_arr(j);
   for (unsigned i = 0; i < intr->num_srcs; i++) {
      const struct own_ir_intrinsic_src *s = &intr->src[i];
      jw_sep(j);
      jw_begin_obj(j);
      jw_src_const_fields(j, s->ssa, s->is_const, s->const_components,
                          s->const_bit_size, s->const_u32);
      jw_key_str(j, "alu_type", alu_type_name(s->alu_type));
      jw_end_obj(j);
   }
   jw_end_arr(j);
   jw_end_obj(j);
}

static void
jw_load_const(struct jw *j, const struct own_ir_load_const *lc)
{
   jw_begin_obj(j);
   jw_key(j, "dest");
   jw_value(j, &lc->dest);
   jw_key(j, "raw");
   jw_u32_vec(j, lc->raw, lc->dest.num_components);
   jw_end_obj(j);
}

static void
jw_tex(struct jw *j, const struct own_ir_tex *tex)
{
   jw_begin_obj(j);
   jw_key(j, "dest");
   jw_value(j, &tex->dest);
   jw_key_str(j, "dest_type", alu_type_name(tex->dest_type));
   jw_key_str(j, "op", texop_name(tex->op));
   jw_key_str(j, "sampler_dim", sampler_dim_enum(tex->sampler_dim));
   jw_key_bool(j, "is_array", tex->is_array);
   jw_key_bool(j, "is_shadow", tex->is_shadow);
   jw_key_bool(j, "is_sparse", tex->is_sparse);
   jw_key_u(j, "component", tex->component);
   jw_key_bool(j, "is_gather_implicit_lod", tex->is_gather_implicit_lod);
   jw_key_bool(j, "skip_helpers", tex->skip_helpers);
   jw_key_bool(j, "texture_non_uniform", tex->texture_non_uniform);
   jw_key_bool(j, "sampler_non_uniform", tex->sampler_non_uniform);
   jw_key_u(j, "texture_index", tex->texture_index);
   jw_key_u(j, "sampler_index", tex->sampler_index);
   jw_key_bool(j, "need_sampler", tex->need_sampler);
   jw_key_bool(j, "has_explicit_tg4_offsets", tex->has_explicit_tg4_offsets);
   jw_key_u(j, "backend_flags", tex->backend_flags);
   if (tex->has_explicit_tg4_offsets) {
      jw_key(j, "tg4_offsets");
      jw_begin_arr(j);
      for (unsigned i = 0; i < 4; i++) {
         jw_sep(j);
         jw_begin_arr(j);
         jw_sep(j);
         jw_i(j, tex->tg4_offsets[i][0]);
         jw_sep(j);
         jw_i(j, tex->tg4_offsets[i][1]);
         jw_end_arr(j);
      }
      jw_end_arr(j);
   }
   jw_key(j, "srcs");
   jw_begin_arr(j);
   for (unsigned i = 0; i < tex->num_srcs; i++) {
      const struct own_ir_tex_src *s = &tex->src[i];
      jw_sep(j);
      jw_begin_obj(j);
      jw_src_const_fields(j, s->ssa, s->is_const, s->const_components,
                          s->const_bit_size, s->const_u32);
      jw_key_str(j, "src_type", tex_src_type_enum(s->src_type));
      jw_key_str(j, "src_alu_type", alu_type_name(s->src_alu_type));
      jw_end_obj(j);
   }
   jw_end_arr(j);
   jw_end_obj(j);
}

static void
jw_phi(struct jw *j, const struct own_ir_phi *phi)
{
   jw_begin_obj(j);
   jw_key(j, "dest");
   jw_value(j, &phi->dest);
   jw_key(j, "srcs");
   jw_begin_arr(j);
   unsigned n = util_dynarray_num_elements(&phi->srcs, struct own_ir_phi_src);
   struct own_ir_phi_src *srcs =
      util_dynarray_element(&phi->srcs, struct own_ir_phi_src, 0);
   for (unsigned i = 0; i < n; i++) {
      jw_sep(j);
      jw_begin_obj(j);
      jw_key(j, "pred");
      jw_block_label(j, srcs[i].pred);
      jw_src_const_fields(j, srcs[i].ssa, srcs[i].is_const,
                          srcs[i].const_components, srcs[i].const_bit_size,
                          srcs[i].const_u32);
      jw_end_obj(j);
   }
   jw_end_arr(j);
   jw_end_obj(j);
}

static void
jw_instr(struct jw *j, const struct own_ir_instr *instr)
{
   jw_begin_obj(j);
   switch (instr->kind) {
   case OWN_IR_INSTR_ALU:
      jw_key_str(j, "kind", "alu");
      jw_key(j, "alu");
      jw_alu(j, &instr->alu);
      break;
   case OWN_IR_INSTR_DEREF:
      jw_key_str(j, "kind", "deref");
      jw_key(j, "deref");
      jw_deref(j, &instr->deref);
      break;
   case OWN_IR_INSTR_INTRINSIC:
      jw_key_str(j, "kind", "intrinsic");
      jw_key(j, "intrinsic");
      jw_intrinsic(j, &instr->intrinsic);
      break;
   case OWN_IR_INSTR_LOAD_CONST:
      jw_key_str(j, "kind", "load_const");
      jw_key(j, "load_const");
      jw_load_const(j, &instr->load_const);
      break;
   case OWN_IR_INSTR_JUMP:
      jw_key_str(j, "kind", "jump");
      jw_key(j, "jump");
      jw_begin_obj(j);
      jw_key_str(j, "type", jump_type_name(instr->jump.type));
      jw_end_obj(j);
      break;
   case OWN_IR_INSTR_TEX:
      jw_key_str(j, "kind", "tex");
      jw_key(j, "tex");
      jw_tex(j, &instr->tex);
      break;
   case OWN_IR_INSTR_PHI:
      jw_key_str(j, "kind", "phi");
      jw_key(j, "phi");
      jw_phi(j, &instr->phi);
      break;
   case OWN_IR_INSTR_UNDEF:
      jw_key_str(j, "kind", "undef");
      jw_key(j, "undef");
      jw_begin_obj(j);
      jw_key(j, "dest");
      jw_value(j, &instr->undef.dest);
      jw_end_obj(j);
      break;
   case OWN_IR_INSTR_UNIMPLEMENTED:
      jw_key_str(j, "kind", "unimplemented");
      jw_key(j, "unimplemented");
      jw_begin_obj(j);
      jw_key_str(j, "type", instr_type_name(instr->unimplemented.type));
      jw_end_obj(j);
      break;
   }
   jw_end_obj(j);
}

static void
jw_block_labels(struct jw *j, const struct util_dynarray *arr)
{
   jw_begin_arr(j);
   unsigned n = util_dynarray_num_elements(arr, unsigned);
   unsigned *v = n ? util_dynarray_element(arr, unsigned, 0) : NULL;
   for (unsigned i = 0; i < n; i++) {
      jw_sep(j);
      jw_block_label(j, v[i]);
   }
   jw_end_arr(j);
}

static void
jw_block(struct jw *j, const struct own_ir_block *block)
{
   jw_begin_obj(j);
   jw_key_u(j, "index", block->index);
   jw_key(j, "label");
   jw_block_label(j, block->index);
   if (block->is_end_block)
      jw_key_bool(j, "is_end_block", true);
   jw_key(j, "preds");
   jw_block_labels(j, &block->preds);
   jw_key(j, "succs");
   jw_block_labels(j, &block->succs);
   jw_key(j, "instrs");
   jw_begin_arr(j);
   util_dynarray_foreach(&block->instrs, struct own_ir_instr *, pip) {
      jw_sep(j);
      jw_instr(j, *pip);
   }
   jw_end_arr(j);
   jw_end_obj(j);
}

static void jw_cf_list(struct jw *j, const struct util_dynarray *list);

static void
jw_if(struct jw *j, const struct own_ir_if *nif)
{
   jw_begin_obj(j);
   jw_key(j, "cond");
   jw_begin_obj(j);
   jw_src_const_fields(j, nif->cond_ssa, nif->cond_is_const,
                       nif->const_components, nif->const_bit_size,
                       nif->const_u32);
   jw_end_obj(j);
   jw_key_str(j, "control", sel_control_name(nif->control));
   jw_key(j, "then");
   jw_cf_list(j, &nif->then_list);
   jw_key(j, "else");
   jw_cf_list(j, &nif->else_list);
   jw_end_obj(j);
}

static void
jw_loop(struct jw *j, const struct own_ir_loop *loop)
{
   jw_begin_obj(j);
   jw_key_str(j, "control", loop_control_name(loop->control));
   jw_key_bool(j, "has_continue", loop->has_continue);
   jw_key(j, "body");
   jw_cf_list(j, &loop->body);
   if (loop->has_continue) {
      jw_key(j, "continue");
      jw_cf_list(j, &loop->continue_list);
   }
   jw_end_obj(j);
}

static void
jw_cf_list(struct jw *j, const struct util_dynarray *list)
{
   jw_begin_arr(j);
   util_dynarray_foreach(list, struct own_ir_cf_node *, pnode) {
      struct own_ir_cf_node *node = *pnode;
      jw_sep(j);
      jw_begin_obj(j);
      switch (node->kind) {
      case OWN_IR_CF_BLOCK:
         jw_key_str(j, "kind", "block");
         jw_key(j, "block");
         jw_block(j, node->block);
         break;
      case OWN_IR_CF_IF:
         jw_key_str(j, "kind", "if");
         jw_key(j, "if");
         jw_if(j, node->nif);
         break;
      case OWN_IR_CF_LOOP:
         jw_key_str(j, "kind", "loop");
         jw_key(j, "loop");
         jw_loop(j, node->loop);
         break;
      }
      jw_end_obj(j);
   }
   jw_end_arr(j);
}

static void
jw_function(struct jw *j, const struct own_ir_function *fn)
{
   jw_begin_obj(j);
   jw_key_str(j, "name", fn->name);
   jw_key_bool(j, "is_entrypoint", fn->is_entrypoint);
   jw_key_u(j, "ssa_alloc", fn->ssa_alloc);
   jw_key(j, "locals");
   jw_begin_arr(j);
   util_dynarray_foreach(&fn->locals, struct own_ir_var *, pv) {
      jw_sep(j);
      jw_var(j, *pv);
   }
   jw_end_arr(j);
   jw_key(j, "body");
   jw_cf_list(j, &fn->body);
   if (fn->end_block) {
      jw_key(j, "end_block");
      jw_block(j, fn->end_block);
   }
   jw_end_obj(j);
}

void
own_ir_print_json(struct nir_shader *nir, FILE *fp, const char *api)
{
   struct own_ir_module *m = own_ir_walk(nir);
   struct jw j = { .fp = fp };

   if (!api || !api[0])
      api = "vulkan";

   jw_begin_obj(&j);
   jw_key_u(&j, "schema_version", 1);
   jw_key_str(&j, "mesa_git", m->mesa_version ? m->mesa_version : own_ir_mesa_version());
   jw_key_str(&j, "stage", mesa_shader_stage_name(m->stage));
    jw_key_str(&j, "api", api);
    if (m->name)
       jw_key_str(&j, "name", m->name);
   jw_key(&j, "source_blake3");
   jw_begin_arr(&j);
   for (unsigned i = 0; i < 32; i++) {
      jw_sep(&j);
      jw_u(&j, m->source_blake3[i]);
   }
   jw_end_arr(&j);
   jw_key_u(&j, "max_subgroup_size", m->max_subgroup_size);
   jw_key_u(&j, "min_subgroup_size", m->min_subgroup_size);
    jw_key_bool(&j, "origin_upper_left", m->origin_upper_left);
    if (m->depth_layout)
       jw_key_u(&j, "depth_layout", m->depth_layout);
   jw_key(&j, "workgroup_size");
   jw_begin_arr(&j);
   for (unsigned i = 0; i < 3; i++) {
      jw_sep(&j);
      jw_u(&j, m->workgroup_size[i]);
   }
   jw_end_arr(&j);
   jw_key_bool(&j, "workgroup_size_variable", m->workgroup_size_variable);
   jw_key_u(&j, "num_inputs", m->num_inputs);
   jw_key_u(&j, "num_outputs", m->num_outputs);
   jw_key_u(&j, "num_uniforms", m->num_uniforms);
   jw_key_u(&j, "num_textures", m->num_textures);
   jw_key_u(&j, "num_ubos", m->num_ubos);
   jw_key_u(&j, "num_abos", m->num_abos);
   jw_key_u(&j, "num_ssbos", m->num_ssbos);
   jw_key_u(&j, "num_images", m->num_images);
   jw_key_u(&j, "scratch_size", m->scratch_size);
   jw_key(&j, "variables");
   jw_begin_arr(&j);
   util_dynarray_foreach(&m->variables, struct own_ir_var *, pv) {
      jw_sep(&j);
      jw_var(&j, *pv);
   }
   jw_end_arr(&j);
   jw_key(&j, "functions");
   jw_begin_arr(&j);
   util_dynarray_foreach(&m->functions, struct own_ir_function *, pfn) {
      jw_sep(&j);
      jw_function(&j, *pfn);
   }
   jw_end_arr(&j);
    jw_end_obj(&j);
    fputc('\n', fp);
}

static const char *
jr_strdup(void *mem_ctx, const char *s)
{
   return ralloc_strdup(mem_ctx, s ? s : "");
}

static unsigned
jr_u(const JSON_Object *o, const char *k)
{
   return (unsigned)json_object_get_number(o, k);
}

static int
jr_i(const JSON_Object *o, const char *k)
{
   return (int)json_object_get_number(o, k);
}

static int64_t
jr_i64(const JSON_Object *o, const char *k)
{
   return (int64_t)json_object_get_number(o, k);
}

static bool
jr_b(const JSON_Object *o, const char *k)
{
   return json_object_get_boolean(o, k) == 1;
}

static unsigned
jr_block_index(const char *s)
{
   if (!s || s[0] != 'b')
      return 0;
   return (unsigned)strtoul(s + 1, NULL, 10);
}

static void
jr_u32_arr(const JSON_Array *a, uint32_t *dst, unsigned max)
{
   if (!a)
      return;
   size_t n = json_array_get_count(a);
   if (n > max)
      n = max;
   for (size_t i = 0; i < n; i++)
      dst[i] = (uint32_t)json_array_get_number(a, i);
}

static void
jr_value(const JSON_Object *o, struct own_ir_value *v)
{
   if (!o)
      return;
   v->index = jr_u(o, "index");
   v->bit_size = (uint8_t)json_object_get_number(o, "bit_size");
   v->num_components = (uint8_t)json_object_get_number(o, "num_components");
}

static void
jr_src_const(const JSON_Object *o, unsigned *ssa, bool *is_const,
             uint8_t *nc, uint8_t *bs, uint32_t *u32)
{
   *ssa = jr_u(o, "ssa");
   *is_const = jr_b(o, "is_const");
   if (*is_const) {
      *nc = (uint8_t)json_object_get_number(o, "const_components");
      *bs = (uint8_t)json_object_get_number(o, "const_bit_size");
      jr_u32_arr(json_object_get_array(o, "const_u32"), u32, NIR_MAX_VEC_COMPONENTS);
   }
}

static nir_variable_mode
parse_var_mode(const char *s)
{
   if (!s)
      return (nir_variable_mode)0;
#define M(x) if (!strcmp(s, #x)) return x
   M(nir_var_system_value);
   M(nir_var_uniform);
   M(nir_var_shader_in);
   M(nir_var_shader_out);
   M(nir_var_image);
   M(nir_var_shader_call_data);
   M(nir_var_ray_hit_attrib);
   M(nir_var_mem_ubo);
   M(nir_var_mem_push_const);
   M(nir_var_mem_ssbo);
   M(nir_var_mem_constant);
   M(nir_var_mem_task_payload);
   M(nir_var_mem_node_payload);
   M(nir_var_mem_node_payload_in);
   M(nir_var_mem_pixel_local_in);
   M(nir_var_mem_pixel_local_out);
   M(nir_var_mem_pixel_local_inout);
   M(nir_var_function_in);
   M(nir_var_function_out);
   M(nir_var_function_inout);
   M(nir_var_resource_heap);
   M(nir_var_sampler_heap);
   M(nir_var_shader_temp);
   M(nir_var_function_temp);
   M(nir_var_mem_shared);
   M(nir_var_mem_global);
#undef M
   return (nir_variable_mode)0;
}

static enum glsl_interp_mode
parse_interp(const char *s)
{
   if (!s)
      return INTERP_MODE_NONE;
   if (!strcmp(s, "INTERP_MODE_SMOOTH"))
      return INTERP_MODE_SMOOTH;
   if (!strcmp(s, "INTERP_MODE_FLAT"))
      return INTERP_MODE_FLAT;
   if (!strcmp(s, "INTERP_MODE_NOPERSPECTIVE"))
      return INTERP_MODE_NOPERSPECTIVE;
   if (!strcmp(s, "INTERP_MODE_EXPLICIT"))
      return INTERP_MODE_EXPLICIT;
   return INTERP_MODE_NONE;
}

static unsigned
parse_access_name(const char *s)
{
   if (!s)
      return 0;
#define M(x) if (!strcmp(s, #x)) return x
   M(ACCESS_COHERENT);
   M(ACCESS_RESTRICT);
   M(ACCESS_VOLATILE);
   M(ACCESS_NON_WRITEABLE);
   M(ACCESS_NON_READABLE);
   M(ACCESS_NON_UNIFORM);
   M(ACCESS_CAN_REORDER);
   M(ACCESS_NON_TEMPORAL);
   M(ACCESS_INCLUDE_HELPERS);
   M(ACCESS_IS_SWIZZLED_AMD);
   M(ACCESS_USES_FORMAT_AMD);
   M(ACCESS_FMASK_LOWERED_AMD);
   M(ACCESS_CAN_SPECULATE);
   M(ACCESS_CP_GE_COHERENT_AMD);
   M(ACCESS_IN_BOUNDS);
   M(ACCESS_KEEP_SCALAR);
   M(ACCESS_SMEM_AMD);
   M(ACCESS_SKIP_HELPERS);
   M(ACCESS_ATOMIC);
   M(ACCESS_FUSED_EU_DISABLE_INTEL);
   M(ACCESS_SPARSE);
   M(ACCESS_ISTREAM_PAN);
   M(ACCESS_ESTREAM_PAN);
#undef M
   return 0;
}

static unsigned
parse_access_arr(const JSON_Array *a)
{
   unsigned access = 0;
   if (!a)
      return 0;
   size_t n = json_array_get_count(a);
   for (size_t i = 0; i < n; i++)
      access |= parse_access_name(json_array_get_string(a, i));
   return access;
}

static nir_variable_mode
parse_modes_arr(const JSON_Array *a)
{
   nir_variable_mode modes = 0;
   if (!a)
      return 0;
   size_t n = json_array_get_count(a);
   for (size_t i = 0; i < n; i++)
      modes |= parse_var_mode(json_array_get_string(a, i));
   return modes;
}

static nir_alu_type
parse_alu_type(const char *s)
{
   if (!s)
      return nir_type_invalid;
#define M(x) if (!strcmp(s, #x)) return x
   M(nir_type_invalid);
   M(nir_type_int);
   M(nir_type_uint);
   M(nir_type_bool);
   M(nir_type_float);
   M(nir_type_bool1);
   M(nir_type_bool32);
   M(nir_type_int1);
   M(nir_type_int8);
   M(nir_type_int16);
   M(nir_type_int32);
   M(nir_type_int64);
   M(nir_type_uint1);
   M(nir_type_uint8);
   M(nir_type_uint16);
   M(nir_type_uint32);
   M(nir_type_uint64);
   M(nir_type_float16);
   M(nir_type_float32);
   M(nir_type_float64);
#undef M
   return nir_type_invalid;
}

static nir_deref_type
parse_deref_type(const char *s)
{
   if (!s)
      return nir_deref_type_var;
   if (!strcmp(s, "nir_deref_type_array"))
      return nir_deref_type_array;
   if (!strcmp(s, "nir_deref_type_array_wildcard"))
      return nir_deref_type_array_wildcard;
   if (!strcmp(s, "nir_deref_type_ptr_as_array"))
      return nir_deref_type_ptr_as_array;
   if (!strcmp(s, "nir_deref_type_struct"))
      return nir_deref_type_struct;
   if (!strcmp(s, "nir_deref_type_cast"))
      return nir_deref_type_cast;
   return nir_deref_type_var;
}

static nir_jump_type
parse_jump_type(const char *s)
{
   if (!s)
      return nir_jump_return;
   if (!strcmp(s, "nir_jump_halt"))
      return nir_jump_halt;
   if (!strcmp(s, "nir_jump_abort"))
      return nir_jump_abort;
   if (!strcmp(s, "nir_jump_break"))
      return nir_jump_break;
   if (!strcmp(s, "nir_jump_continue"))
      return nir_jump_continue;
   if (!strcmp(s, "nir_jump_goto"))
      return nir_jump_goto;
   if (!strcmp(s, "nir_jump_goto_if"))
      return nir_jump_goto_if;
   return nir_jump_return;
}

static nir_texop
parse_texop(const char *s)
{
   if (!s)
      return nir_texop_tex;
#define M(x) if (!strcmp(s, #x)) return x
   M(nir_texop_tex);
   M(nir_texop_txb);
   M(nir_texop_txl);
   M(nir_texop_txd);
   M(nir_texop_txf);
   M(nir_texop_txf_ms);
   M(nir_texop_txf_ms_fb);
   M(nir_texop_txf_ms_mcs_intel);
   M(nir_texop_txs);
   M(nir_texop_lod);
   M(nir_texop_tg4);
   M(nir_texop_query_levels);
   M(nir_texop_texture_samples);
   M(nir_texop_samples_identical);
   M(nir_texop_tex_prefetch);
   M(nir_texop_lod_bias);
   M(nir_texop_fragment_fetch_amd);
   M(nir_texop_fragment_mask_fetch_amd);
   M(nir_texop_descriptor_amd);
   M(nir_texop_sampler_descriptor_amd);
   M(nir_texop_image_min_lod_agx);
   M(nir_texop_has_custom_border_color_agx);
   M(nir_texop_custom_border_color_agx);
   M(nir_texop_hdr_dim_nv);
   M(nir_texop_tex_type_nv);
   M(nir_texop_sample_pos_nv);
   M(nir_texop_gradient_pan);
   M(nir_texop_sample_weighted_qcom);
   M(nir_texop_box_filter_qcom);
   M(nir_texop_block_match_sad_qcom);
   M(nir_texop_block_match_ssd_qcom);
   M(nir_texop_resinfo_intel);
   M(nir_texop_sparse_residency_intel);
   M(nir_texop_sparse_residency_txf_intel);
#undef M
   return nir_texop_tex;
}

static nir_tex_src_type
parse_tex_src_type(const char *s)
{
   if (!s)
      return nir_tex_src_coord;
#define M(x) if (!strcmp(s, #x)) return x
   M(nir_tex_src_coord);
   M(nir_tex_src_projector);
   M(nir_tex_src_comparator);
   M(nir_tex_src_offset);
   M(nir_tex_src_bias);
   M(nir_tex_src_lod);
   M(nir_tex_src_min_lod);
   M(nir_tex_src_max_lod_kk);
   M(nir_tex_src_lod_bias_min_agx);
   M(nir_tex_src_ms_index);
   M(nir_tex_src_ms_mcs_intel);
   M(nir_tex_src_ddx);
   M(nir_tex_src_ddy);
   M(nir_tex_src_texture_deref);
   M(nir_tex_src_sampler_deref);
   M(nir_tex_src_texture_offset);
   M(nir_tex_src_sampler_offset);
   M(nir_tex_src_texture_handle);
   M(nir_tex_src_sampler_handle);
   M(nir_tex_src_texture_heap_offset);
   M(nir_tex_src_sampler_heap_offset);
   M(nir_tex_src_sampler_deref_intrinsic);
   M(nir_tex_src_texture_deref_intrinsic);
   M(nir_tex_src_plane);
   M(nir_tex_src_backend1);
   M(nir_tex_src_backend2);
   M(nir_tex_src_ref_coord);
   M(nir_tex_src_texture_2_deref);
   M(nir_tex_src_sampler_2_deref);
   M(nir_tex_src_texture_2_handle);
   M(nir_tex_src_sampler_2_handle);
   M(nir_tex_src_block_size);
   M(nir_tex_src_box_size);
#undef M
   return nir_tex_src_coord;
}

static enum glsl_sampler_dim
parse_sampler_dim(const char *s)
{
   if (!s)
      return GLSL_SAMPLER_DIM_2D;
#define M(x) if (!strcmp(s, #x)) return x
   M(GLSL_SAMPLER_DIM_1D);
   M(GLSL_SAMPLER_DIM_2D);
   M(GLSL_SAMPLER_DIM_3D);
   M(GLSL_SAMPLER_DIM_CUBE);
   M(GLSL_SAMPLER_DIM_RECT);
   M(GLSL_SAMPLER_DIM_BUF);
   M(GLSL_SAMPLER_DIM_EXTERNAL);
   M(GLSL_SAMPLER_DIM_MS);
   M(GLSL_SAMPLER_DIM_SUBPASS);
   M(GLSL_SAMPLER_DIM_SUBPASS_MS);
#undef M
   return GLSL_SAMPLER_DIM_2D;
}

static nir_selection_control
parse_sel_control(const char *s)
{
   if (!s)
      return nir_selection_control_none;
   if (!strcmp(s, "nir_selection_control_flatten"))
      return nir_selection_control_flatten;
   if (!strcmp(s, "nir_selection_control_dont_flatten"))
      return nir_selection_control_dont_flatten;
   if (!strcmp(s, "nir_selection_control_divergent_always_taken"))
      return nir_selection_control_divergent_always_taken;
   return nir_selection_control_none;
}

static nir_loop_control
parse_loop_control(const char *s)
{
   if (!s)
      return nir_loop_control_none;
   if (!strcmp(s, "nir_loop_control_unroll"))
      return nir_loop_control_unroll;
   if (!strcmp(s, "nir_loop_control_dont_unroll"))
      return nir_loop_control_dont_unroll;
   return nir_loop_control_none;
}

static nir_instr_type
parse_instr_type(const char *s)
{
   if (!s)
      return nir_instr_type_alu;
#define M(x) if (!strcmp(s, #x)) return x
   M(nir_instr_type_alu);
   M(nir_instr_type_deref);
   M(nir_instr_type_call);
   M(nir_instr_type_tex);
   M(nir_instr_type_intrinsic);
   M(nir_instr_type_load_const);
   M(nir_instr_type_jump);
   M(nir_instr_type_undef);
   M(nir_instr_type_phi);
   M(nir_instr_type_cmat_call);
#undef M
   return nir_instr_type_alu;
}

static nir_op
parse_alu_op(const char *s)
{
   if (!s)
      return (nir_op)0;
   for (unsigned i = 0; i < nir_num_opcodes; i++) {
      if (!strcmp(nir_op_infos[i].name, s))
         return (nir_op)i;
   }
   return (nir_op)0;
}

static nir_intrinsic_op
parse_intrinsic_op(const char *s)
{
   if (!s)
      return (nir_intrinsic_op)0;
   for (unsigned i = 0; i < nir_num_intrinsics; i++) {
      if (!strcmp(nir_intrinsic_infos[i].name, s))
         return (nir_intrinsic_op)i;
   }
   return (nir_intrinsic_op)0;
}

static mesa_shader_stage
parse_stage(const char *s)
{
   if (!s)
      return MESA_SHADER_NONE;
   for (unsigned i = 0; i < MESA_ALL_SHADER_STAGES; i++) {
      if (!strcmp(mesa_shader_stage_name((mesa_shader_stage)i), s))
         return (mesa_shader_stage)i;
   }
   return MESA_SHADER_NONE;
}

static unsigned
infer_type_components(const char *type_name)
{
   if (!type_name || !type_name[0])
      return 0;
   if (!strncmp(type_name, "vec4", 4) || !strncmp(type_name, "ivec4", 5) ||
       !strncmp(type_name, "uvec4", 5) || !strncmp(type_name, "bvec4", 5))
      return 4;
   if (!strncmp(type_name, "vec3", 4) || !strncmp(type_name, "ivec3", 5) ||
       !strncmp(type_name, "uvec3", 5) || !strncmp(type_name, "bvec3", 5))
      return 3;
   if (!strncmp(type_name, "vec2", 4) || !strncmp(type_name, "ivec2", 5) ||
       !strncmp(type_name, "uvec2", 5) || !strncmp(type_name, "bvec2", 5))
      return 2;
   if (!strcmp(type_name, "float") || !strcmp(type_name, "int") ||
       !strcmp(type_name, "uint") || !strcmp(type_name, "bool") ||
       !strncmp(type_name, "sampler", 7))
      return 1;
   return 0;
}

static void
jr_var(void *mem_ctx, const JSON_Object *o, struct own_ir_var *var)
{
   var->mode = parse_var_mode(json_object_get_string(o, "mode"));
   var->interpolation = parse_interp(json_object_get_string(o, "interpolation"));
   var->type = NULL;
   {
      JSON_Value *tv = json_object_get_value(o, "type");
      const char *ts = NULL;
      if (tv && json_value_get_type(tv) == JSONString)
         ts = json_value_get_string(tv);
      var->type_name = jr_strdup(mem_ctx, ts);
   }
   if (json_object_has_value(o, "type_components"))
      var->type_components = jr_u(o, "type_components");
   else
      var->type_components = infer_type_components(var->type_name);
   var->name = jr_strdup(mem_ctx, json_object_get_string(o, "name"));
   var->location = jr_i(o, "location");
   var->location_frac = jr_u(o, "location_frac");
   var->component_mask = jr_u(o, "component_mask");
   var->driver_location = jr_u(o, "driver_location");
    var->binding = jr_u(o, "binding");
    var->access = parse_access_arr(json_object_get_array(o, "access"));
    if (json_object_has_value(o, "image_format"))
       var->image_format_name =
          jr_strdup(mem_ctx, json_object_get_string(o, "image_format"));
    else
       var->image_format_name = NULL;
}

static void
jr_alu(void *mem_ctx, const JSON_Object *o, struct own_ir_alu *alu)
{
   (void)mem_ctx;
   jr_value(json_object_get_object(o, "dest"), &alu->dest);
   alu->op = parse_alu_op(json_object_get_string(o, "op"));
   alu->no_signed_wrap = jr_b(o, "no_signed_wrap");
   alu->no_unsigned_wrap = jr_b(o, "no_unsigned_wrap");
   alu->fp_math_ctrl = jr_u(o, "fp_math_ctrl");
   JSON_Array *srcs = json_object_get_array(o, "srcs");
   unsigned n = srcs ? (unsigned)json_array_get_count(srcs) : 0;
   if (n > NIR_ALU_MAX_INPUTS)
      n = NIR_ALU_MAX_INPUTS;
   alu->num_srcs = n;
   for (unsigned i = 0; i < n; i++) {
      JSON_Object *so = json_array_get_object(srcs, i);
      struct own_ir_alu_src *s = &alu->src[i];
      jr_src_const(so, &s->ssa, &s->is_const, &s->const_components,
                   &s->const_bit_size, s->const_u32);
      s->src_components = (uint8_t)json_object_get_number(so, "src_components");
      JSON_Array *sw = json_object_get_array(so, "swizzle");
      if (sw) {
         size_t sn = json_array_get_count(sw);
         if (sn > NIR_MAX_VEC_COMPONENTS)
            sn = NIR_MAX_VEC_COMPONENTS;
         for (size_t c = 0; c < sn; c++)
            s->swizzle[c] = (uint8_t)json_array_get_number(sw, c);
      }
   }
}

static void
jr_deref(void *mem_ctx, const JSON_Object *o, struct own_ir_deref *d)
{
   jr_value(json_object_get_object(o, "dest"), &d->dest);
   d->deref_type = parse_deref_type(json_object_get_string(o, "deref_type"));
   d->modes = parse_modes_arr(json_object_get_array(o, "modes"));
   d->type = NULL;
   {
      JSON_Value *tv = json_object_get_value(o, "type");
      const char *ts = NULL;
      if (tv && json_value_get_type(tv) == JSONString)
         ts = json_value_get_string(tv);
      d->type_name = jr_strdup(mem_ctx, ts);
   }
   d->var_name = jr_strdup(mem_ctx, json_object_get_string(o, "var_name"));
   d->parent_ssa = jr_u(o, "parent_ssa");
   d->struct_index = jr_u(o, "struct_index");
   d->struct_field = jr_strdup(mem_ctx, json_object_get_string(o, "struct_field"));
   d->arr_in_bounds = jr_b(o, "arr_in_bounds");
   d->arr_index_is_const = jr_b(o, "arr_index_is_const");
   d->arr_index_const = jr_i64(o, "arr_index_const");
   d->arr_index_ssa = jr_u(o, "arr_index_ssa");
   d->ptr_stride = jr_u(o, "ptr_stride");
   d->align_mul = jr_u(o, "align_mul");
   d->align_offset = jr_u(o, "align_offset");
   d->chain_comment = jr_strdup(mem_ctx, json_object_get_string(o, "chain_comment"));
}

static void
jr_intrinsic(void *mem_ctx, const JSON_Object *o, struct own_ir_intrinsic *intr)
{
   (void)mem_ctx;
   intr->has_dest = jr_b(o, "has_dest");
   if (intr->has_dest)
      jr_value(json_object_get_object(o, "dest"), &intr->dest);
   intr->dest_alu_type = parse_alu_type(json_object_get_string(o, "dest_alu_type"));
   intr->op = parse_intrinsic_op(json_object_get_string(o, "op"));
   {
      const nir_intrinsic_info *info = &nir_intrinsic_infos[intr->op];
      intr->name = info->name;
   }
   intr->num_components = (uint8_t)json_object_get_number(o, "num_components");
   intr->write_mask = jr_u(o, "write_mask");
   intr->access = parse_access_arr(json_object_get_array(o, "access"));
   {
      JSON_Array *ci = json_object_get_array(o, "const_index");
      if (ci) {
         size_t n = json_array_get_count(ci);
         if (n > NIR_INTRINSIC_MAX_CONST_INDEX)
            n = NIR_INTRINSIC_MAX_CONST_INDEX;
         for (size_t i = 0; i < n; i++)
            intr->const_index[i] = (int)json_array_get_number(ci, i);
      }
   }
   JSON_Array *srcs = json_object_get_array(o, "srcs");
   unsigned n = srcs ? (unsigned)json_array_get_count(srcs) : 0;
   if (n > OWN_IR_MAX_SRCS)
      n = OWN_IR_MAX_SRCS;
   intr->num_srcs = n;
   for (unsigned i = 0; i < n; i++) {
      JSON_Object *so = json_array_get_object(srcs, i);
      struct own_ir_intrinsic_src *s = &intr->src[i];
      jr_src_const(so, &s->ssa, &s->is_const, &s->const_components,
                   &s->const_bit_size, s->const_u32);
      s->alu_type = parse_alu_type(json_object_get_string(so, "alu_type"));
   }
}

static void
jr_load_const(const JSON_Object *o, struct own_ir_load_const *lc)
{
   jr_value(json_object_get_object(o, "dest"), &lc->dest);
   jr_u32_arr(json_object_get_array(o, "raw"), lc->raw, NIR_MAX_VEC_COMPONENTS);
}

static void
jr_tex(const JSON_Object *o, struct own_ir_tex *tex)
{
   jr_value(json_object_get_object(o, "dest"), &tex->dest);
   tex->dest_type = parse_alu_type(json_object_get_string(o, "dest_type"));
   tex->op = parse_texop(json_object_get_string(o, "op"));
   tex->sampler_dim = parse_sampler_dim(json_object_get_string(o, "sampler_dim"));
   tex->is_array = jr_b(o, "is_array");
   tex->is_shadow = jr_b(o, "is_shadow");
   tex->is_sparse = jr_b(o, "is_sparse");
   tex->component = jr_u(o, "component");
   tex->is_gather_implicit_lod = jr_b(o, "is_gather_implicit_lod");
   tex->skip_helpers = jr_b(o, "skip_helpers");
   tex->texture_non_uniform = jr_b(o, "texture_non_uniform");
   tex->sampler_non_uniform = jr_b(o, "sampler_non_uniform");
   tex->texture_index = jr_u(o, "texture_index");
   tex->sampler_index = jr_u(o, "sampler_index");
   tex->need_sampler = jr_b(o, "need_sampler");
   tex->has_explicit_tg4_offsets = jr_b(o, "has_explicit_tg4_offsets");
   tex->backend_flags = jr_u(o, "backend_flags");
   {
      JSON_Array *off = json_object_get_array(o, "tg4_offsets");
      if (off) {
         size_t n = json_array_get_count(off);
         if (n > 4)
            n = 4;
         for (size_t i = 0; i < n; i++) {
            JSON_Array *row = json_array_get_array(off, i);
            if (!row || json_array_get_count(row) < 2)
               continue;
            tex->tg4_offsets[i][0] = (int8_t)json_array_get_number(row, 0);
            tex->tg4_offsets[i][1] = (int8_t)json_array_get_number(row, 1);
         }
      }
   }
   JSON_Array *srcs = json_object_get_array(o, "srcs");
   unsigned n = srcs ? (unsigned)json_array_get_count(srcs) : 0;
   if (n > OWN_IR_MAX_SRCS)
      n = OWN_IR_MAX_SRCS;
   tex->num_srcs = n;
   for (unsigned i = 0; i < n; i++) {
      JSON_Object *so = json_array_get_object(srcs, i);
      struct own_ir_tex_src *s = &tex->src[i];
      jr_src_const(so, &s->ssa, &s->is_const, &s->const_components,
                   &s->const_bit_size, s->const_u32);
      s->src_type = parse_tex_src_type(json_object_get_string(so, "src_type"));
      s->src_alu_type = parse_alu_type(json_object_get_string(so, "src_alu_type"));
   }
}

static void
jr_phi(void *mem_ctx, const JSON_Object *o, struct own_ir_phi *phi)
{
   jr_value(json_object_get_object(o, "dest"), &phi->dest);
   util_dynarray_init(&phi->srcs, mem_ctx);
   JSON_Array *srcs = json_object_get_array(o, "srcs");
   if (!srcs)
      return;
   size_t n = json_array_get_count(srcs);
   for (size_t i = 0; i < n; i++) {
      JSON_Object *so = json_array_get_object(srcs, i);
      struct own_ir_phi_src ps = { 0 };
      ps.pred = jr_block_index(json_object_get_string(so, "pred"));
      jr_src_const(so, &ps.ssa, &ps.is_const, &ps.const_components,
                   &ps.const_bit_size, ps.const_u32);
      util_dynarray_append(&phi->srcs, ps);
   }
}

static void
jr_instr(void *mem_ctx, const JSON_Object *o, struct own_ir_instr *instr)
{
   const char *kind = json_object_get_string(o, "kind");
   if (!kind)
      return;
   if (!strcmp(kind, "alu")) {
      instr->kind = OWN_IR_INSTR_ALU;
      jr_alu(mem_ctx, json_object_get_object(o, "alu"), &instr->alu);
   } else if (!strcmp(kind, "deref")) {
      instr->kind = OWN_IR_INSTR_DEREF;
      jr_deref(mem_ctx, json_object_get_object(o, "deref"), &instr->deref);
   } else if (!strcmp(kind, "intrinsic")) {
      instr->kind = OWN_IR_INSTR_INTRINSIC;
      jr_intrinsic(mem_ctx, json_object_get_object(o, "intrinsic"),
                   &instr->intrinsic);
   } else if (!strcmp(kind, "load_const")) {
      instr->kind = OWN_IR_INSTR_LOAD_CONST;
      jr_load_const(json_object_get_object(o, "load_const"), &instr->load_const);
   } else if (!strcmp(kind, "jump")) {
      instr->kind = OWN_IR_INSTR_JUMP;
      JSON_Object *jo = json_object_get_object(o, "jump");
      instr->jump.type = parse_jump_type(jo ? json_object_get_string(jo, "type") : NULL);
   } else if (!strcmp(kind, "tex")) {
      instr->kind = OWN_IR_INSTR_TEX;
      jr_tex(json_object_get_object(o, "tex"), &instr->tex);
   } else if (!strcmp(kind, "phi")) {
      instr->kind = OWN_IR_INSTR_PHI;
      jr_phi(mem_ctx, json_object_get_object(o, "phi"), &instr->phi);
   } else if (!strcmp(kind, "undef")) {
      instr->kind = OWN_IR_INSTR_UNDEF;
      JSON_Object *uo = json_object_get_object(o, "undef");
      jr_value(uo ? json_object_get_object(uo, "dest") : NULL, &instr->undef.dest);
   } else if (!strcmp(kind, "unimplemented")) {
      instr->kind = OWN_IR_INSTR_UNIMPLEMENTED;
      JSON_Object *uo = json_object_get_object(o, "unimplemented");
      instr->unimplemented.type =
         parse_instr_type(uo ? json_object_get_string(uo, "type") : NULL);
   }
}

static void
jr_block_labels(const JSON_Array *a, struct own_ir_block *block, bool preds)
{
   if (!a)
      return;
   size_t n = json_array_get_count(a);
   for (size_t i = 0; i < n; i++) {
      unsigned idx = jr_block_index(json_array_get_string(a, i));
      if (preds)
         own_ir_block_add_pred(block, idx);
      else
         own_ir_block_add_succ(block, idx);
   }
}

static struct own_ir_block *
jr_block(void *mem_ctx, const JSON_Object *o)
{
   struct own_ir_block *block = own_ir_block_create(mem_ctx);
   block->index = jr_u(o, "index");
   block->is_end_block = jr_b(o, "is_end_block");
   jr_block_labels(json_object_get_array(o, "preds"), block, true);
   jr_block_labels(json_object_get_array(o, "succs"), block, false);
   JSON_Array *instrs = json_object_get_array(o, "instrs");
   if (instrs) {
      size_t n = json_array_get_count(instrs);
      for (size_t i = 0; i < n; i++) {
         JSON_Object *io = json_array_get_object(instrs, i);
         struct own_ir_instr *instr = own_ir_block_add_instr(block);
         jr_instr(mem_ctx, io, instr);
      }
   }
   return block;
}

static bool jr_cf_list(void *mem_ctx, const JSON_Array *a, struct util_dynarray *out);

static bool
jr_if(void *mem_ctx, const JSON_Object *o, struct util_dynarray *out)
{
   struct own_ir_if *nif = own_ir_if_create(mem_ctx);
   JSON_Object *cond = json_object_get_object(o, "cond");
   if (cond)
      jr_src_const(cond, &nif->cond_ssa, &nif->cond_is_const,
                   &nif->const_components, &nif->const_bit_size, nif->const_u32);
   nif->control = parse_sel_control(json_object_get_string(o, "control"));
   if (!jr_cf_list(mem_ctx, json_object_get_array(o, "then"), &nif->then_list))
      return false;
   if (!jr_cf_list(mem_ctx, json_object_get_array(o, "else"), &nif->else_list))
      return false;
   util_dynarray_append(out, own_ir_cf_wrap_if(mem_ctx, nif));
   return true;
}

static bool
jr_loop(void *mem_ctx, const JSON_Object *o, struct util_dynarray *out)
{
   struct own_ir_loop *loop = own_ir_loop_create(mem_ctx);
   loop->control = parse_loop_control(json_object_get_string(o, "control"));
   loop->has_continue = jr_b(o, "has_continue");
   if (!jr_cf_list(mem_ctx, json_object_get_array(o, "body"), &loop->body))
      return false;
   if (loop->has_continue) {
      if (!jr_cf_list(mem_ctx, json_object_get_array(o, "continue"),
                      &loop->continue_list))
         return false;
   }
   util_dynarray_append(out, own_ir_cf_wrap_loop(mem_ctx, loop));
   return true;
}

static bool
jr_cf_list(void *mem_ctx, const JSON_Array *a, struct util_dynarray *out)
{
   if (!a)
      return true;
   size_t n = json_array_get_count(a);
   for (size_t i = 0; i < n; i++) {
      JSON_Object *o = json_array_get_object(a, i);
      const char *kind = json_object_get_string(o, "kind");
      if (!kind)
         continue;
      if (!strcmp(kind, "block")) {
         struct own_ir_block *block = jr_block(mem_ctx, json_object_get_object(o, "block"));
         util_dynarray_append(out, own_ir_cf_wrap_block(mem_ctx, block));
      } else if (!strcmp(kind, "if")) {
         if (!jr_if(mem_ctx, json_object_get_object(o, "if"), out))
            return false;
      } else if (!strcmp(kind, "loop")) {
         if (!jr_loop(mem_ctx, json_object_get_object(o, "loop"), out))
            return false;
      }
   }
   return true;
}

static bool
jr_function(void *mem_ctx, const JSON_Object *o, struct own_ir_function *fn)
{
   fn->name = jr_strdup(mem_ctx, json_object_get_string(o, "name"));
   fn->is_entrypoint = jr_b(o, "is_entrypoint");
   fn->ssa_alloc = jr_u(o, "ssa_alloc");
   JSON_Array *locals = json_object_get_array(o, "locals");
   if (locals) {
      size_t n = json_array_get_count(locals);
      for (size_t i = 0; i < n; i++) {
         struct own_ir_var *var = own_ir_function_add_local(fn);
         jr_var(mem_ctx, json_array_get_object(locals, i), var);
      }
   }
   if (!jr_cf_list(mem_ctx, json_object_get_array(o, "body"), &fn->body))
      return false;
   JSON_Object *end = json_object_get_object(o, "end_block");
   if (end)
      fn->end_block = jr_block(mem_ctx, end);
   own_ir_gather_types(fn);
   return true;
}

struct own_ir_module *
own_ir_from_json(void *mem_ctx, const char *path, char **err)
{
   JSON_Value *root = json_parse_file(path);
   if (!root) {
      if (err)
         *err = ralloc_strdup(mem_ctx, "failed to parse JSON file");
      return NULL;
   }

   JSON_Object *o = json_value_get_object(root);
   if (!o) {
      json_value_free(root);
      if (err)
         *err = ralloc_strdup(mem_ctx, "JSON root is not an object");
      return NULL;
   }

   if ((int)json_object_get_number(o, "schema_version") != 1) {
      json_value_free(root);
      if (err)
         *err = ralloc_strdup(mem_ctx, "unsupported schema_version");
      return NULL;
   }

   struct own_ir_module *m = own_ir_module_create(mem_ctx);
   m->mesa_version = jr_strdup(m->mem_ctx, json_object_get_string(o, "mesa_git"));
   m->stage = parse_stage(json_object_get_string(o, "stage"));
    if (json_object_has_value(o, "name")) {
       const char *nm = json_object_get_string(o, "name");
       m->name = nm ? jr_strdup(m->mem_ctx, nm) : NULL;
    } else {
       m->name = NULL;
    }
   {
      JSON_Array *b3 = json_object_get_array(o, "source_blake3");
      if (b3) {
         size_t n = json_array_get_count(b3);
         if (n > 32)
            n = 32;
         for (size_t i = 0; i < n; i++)
            m->source_blake3[i] = (uint8_t)json_array_get_number(b3, i);
      }
   }
   m->max_subgroup_size = (uint8_t)json_object_get_number(o, "max_subgroup_size");
   m->min_subgroup_size = (uint8_t)json_object_get_number(o, "min_subgroup_size");
    m->origin_upper_left = jr_b(o, "origin_upper_left");
    m->depth_layout = json_object_has_value(o, "depth_layout")
                         ? jr_u(o, "depth_layout") : 0;
   {
      JSON_Array *wg = json_object_get_array(o, "workgroup_size");
      if (wg) {
         size_t n = json_array_get_count(wg);
         if (n > 3)
            n = 3;
         for (size_t i = 0; i < n; i++)
            m->workgroup_size[i] = (unsigned)json_array_get_number(wg, i);
      }
   }
   m->workgroup_size_variable = jr_b(o, "workgroup_size_variable");
   m->num_inputs = jr_u(o, "num_inputs");
   m->num_outputs = jr_u(o, "num_outputs");
   m->num_uniforms = jr_u(o, "num_uniforms");
   m->num_textures = jr_u(o, "num_textures");
   m->num_ubos = jr_u(o, "num_ubos");
   m->num_abos = jr_u(o, "num_abos");
   m->num_ssbos = jr_u(o, "num_ssbos");
   m->num_images = jr_u(o, "num_images");
   m->scratch_size = jr_u(o, "scratch_size");

   JSON_Array *vars = json_object_get_array(o, "variables");
   if (vars) {
      size_t n = json_array_get_count(vars);
      for (size_t i = 0; i < n; i++) {
         struct own_ir_var *var = own_ir_module_add_var(m);
         jr_var(m->mem_ctx, json_array_get_object(vars, i), var);
      }
   }

   JSON_Array *fns = json_object_get_array(o, "functions");
   if (fns) {
      size_t n = json_array_get_count(fns);
      for (size_t i = 0; i < n; i++) {
         struct own_ir_function *fn = own_ir_module_add_function(m);
         if (!jr_function(m->mem_ctx, json_array_get_object(fns, i), fn)) {
            json_value_free(root);
            if (err)
               *err = ralloc_strdup(mem_ctx, "failed to parse function");
            return NULL;
         }
      }
   }

   json_value_free(root);
   return m;
}

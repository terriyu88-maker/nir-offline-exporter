#include "ir.h"

#include "util/format/u_format.h"
#include "util/hash_table.h"
#include "util/set.h"

#include <inttypes.h>
#include <string.h>

static void
fill_value(struct own_ir_value *v, const nir_def *def)
{
   v->index = def->index;
   v->bit_size = def->bit_size;
   v->num_components = def->num_components;
}

static const char *
unique_print_name(struct own_ir_module *m, void *ctx, const char *identifier,
                  const char *default_name, struct set *seen, unsigned *index)
{
   struct hash_entry *entry = _mesa_hash_table_search(m->var_print_names, ctx);
   if (entry)
      return entry->data;

   char *name;
   if (identifier == NULL || strlen(identifier) == 0) {
      name = ralloc_asprintf(m->mem_ctx, "%s#%u", default_name, (*index)++);
   } else {
      struct set_entry *set_entry = _mesa_set_search(seen, identifier);
      if (set_entry != NULL) {
         name = ralloc_asprintf(m->mem_ctx, "%s#%u", identifier, (*index)++);
      } else {
         _mesa_set_add(seen, identifier);
         name = (char *)identifier;
      }
   }

   _mesa_hash_table_insert(m->var_print_names, ctx, name);
   return name;
}

static const char *
print_var_name(struct own_ir_module *m, nir_variable *var, struct set *seen,
               unsigned *index)
{
   return unique_print_name(m, var, var->name, "", seen, index);
}

static const char *
lookup_var_print_name(struct own_ir_module *m, nir_variable *var)
{
   if (!var)
      return "";
   struct hash_entry *entry = _mesa_hash_table_search(m->var_print_names, var);
   if (entry)
      return entry->data;
   return var->name ? var->name : "";
}

static void
append_deref_chain(struct own_ir_module *m, char **s,
                   const nir_deref_instr *instr, bool whole_chain)
{
   if (instr->deref_type == nir_deref_type_var) {
      ralloc_strcat(s, lookup_var_print_name(m, instr->var));
      return;
   }
   if (instr->deref_type == nir_deref_type_cast) {
      ralloc_asprintf_append(s, "(%s *)%%%u", glsl_get_type_name(instr->type),
                             instr->parent.ssa->index);
      return;
   }

   nir_deref_instr *parent = nir_def_as_deref(instr->parent.ssa);
   const bool is_parent_cast =
      whole_chain && parent->deref_type == nir_deref_type_cast;
   const bool is_parent_pointer =
      !whole_chain || parent->deref_type == nir_deref_type_cast;
   const bool need_deref =
      is_parent_pointer && instr->deref_type != nir_deref_type_struct;

   if (is_parent_cast || need_deref)
      ralloc_strcat(s, "(");
   if (need_deref)
      ralloc_strcat(s, "*");
   if (whole_chain)
      append_deref_chain(m, s, parent, whole_chain);
   else
      ralloc_asprintf_append(s, "%%%u", instr->parent.ssa->index);
   if (is_parent_cast || need_deref)
      ralloc_strcat(s, ")");

   switch (instr->deref_type) {
   case nir_deref_type_struct:
      ralloc_asprintf_append(s, "%s%s", is_parent_pointer ? "->" : ".",
                             glsl_get_struct_elem_name(parent->type,
                                                       instr->strct.index));
      break;
   case nir_deref_type_array:
   case nir_deref_type_ptr_as_array:
      if (nir_src_is_const(instr->arr.index))
         ralloc_asprintf_append(s, "[%" PRId64 "]",
                                nir_src_as_int(instr->arr.index));
      else
         ralloc_asprintf_append(s, "[%%%u]", instr->arr.index.ssa->index);
      break;
   case nir_deref_type_array_wildcard:
      ralloc_strcat(s, "[*]");
      break;
   default:
      break;
   }
}

static void
fill_var(struct own_ir_var *ov, struct own_ir_module *m, nir_variable *var,
         struct set *seen, unsigned *name_index)
{
   ov->mode = var->data.mode;
   ov->interpolation = var->data.interpolation;
   ov->type = var->type;
   ov->type_name = var->type ? glsl_get_type_name(var->type) : "";
   ov->type_components =
      var->type ? glsl_get_components(glsl_without_array(var->type)) : 0;
   ov->name = print_var_name(m, var, seen, name_index);
   ov->location = var->data.location;
   ov->location_frac = var->data.location_frac;
   ov->driver_location = var->data.driver_location;
    ov->binding = var->data.binding;
    ov->access = var->data.access;
     ov->image_format_name = NULL;
     if (var->type &&
         glsl_get_base_type(glsl_without_array(var->type)) == GLSL_TYPE_IMAGE)
        ov->image_format_name = util_format_short_name(var->data.image.format);
     ov->patch = var->data.patch;
     ov->compact = var->data.compact;

     unsigned num_components = ov->type_components;
   if (num_components > 0 && num_components < 16)
      ov->component_mask = ((1u << num_components) - 1) << var->data.location_frac;
   else
      ov->component_mask = 0;
}

static void
walk_var(struct own_ir_module *m, nir_variable *var, struct set *seen,
         unsigned *name_index)
{
   fill_var(own_ir_module_add_var(m), m, var, seen, name_index);
}

static void
walk_alu(struct own_ir_instr *oi, nir_alu_instr *alu)
{
   oi->kind = OWN_IR_INSTR_ALU;
   fill_value(&oi->alu.dest, &alu->def);
   oi->alu.op = alu->op;
   oi->alu.num_srcs = nir_op_infos[alu->op].num_inputs;
   oi->alu.no_signed_wrap = alu->no_signed_wrap;
   oi->alu.no_unsigned_wrap = alu->no_unsigned_wrap;
   oi->alu.fp_math_ctrl = alu->fp_math_ctrl;

   for (unsigned i = 0; i < oi->alu.num_srcs; i++) {
      struct own_ir_alu_src *asrc = &oi->alu.src[i];
      asrc->ssa = alu->src[i].src.ssa->index;
      memcpy(asrc->swizzle, alu->src[i].swizzle, sizeof(asrc->swizzle));
      asrc->src_components = alu->src[i].src.ssa->num_components;
      asrc->is_const = nir_src_is_const(alu->src[i].src);
      if (asrc->is_const) {
         nir_load_const_instr *load =
            nir_def_as_load_const(alu->src[i].src.ssa);
         unsigned nc = load->def.num_components;
         asrc->const_components = nc;
         asrc->const_bit_size = load->def.bit_size;
         for (unsigned c = 0; c < NIR_MAX_VEC_COMPONENTS; c++) {
            uint8_t sw = alu->src[i].swizzle[c];
            asrc->const_u32[c] = (sw < nc) ? load->value[sw].u32 : 0;
         }
      }
   }
}

static void
walk_deref(struct own_ir_module *m, struct own_ir_instr *oi,
           nir_deref_instr *deref)
{
   oi->kind = OWN_IR_INSTR_DEREF;
   fill_value(&oi->deref.dest, &deref->def);
   oi->deref.deref_type = deref->deref_type;
   oi->deref.modes = deref->modes;
   oi->deref.type = deref->type;
   oi->deref.type_name = deref->type ? glsl_get_type_name(deref->type) : "";
   oi->deref.var_name = "";
   oi->deref.struct_field = "";
   oi->deref.chain_comment = "";

   if (deref->deref_type == nir_deref_type_var) {
      oi->deref.var_name = lookup_var_print_name(m, deref->var);
   } else {
      oi->deref.parent_ssa = deref->parent.ssa->index;
   }

   if (deref->deref_type == nir_deref_type_struct) {
      nir_deref_instr *parent = nir_def_as_deref(deref->parent.ssa);
      oi->deref.struct_index = deref->strct.index;
      oi->deref.struct_field =
         glsl_get_struct_elem_name(parent->type, deref->strct.index);
   }

   if (nir_deref_instr_is_arr(deref)) {
      oi->deref.arr_in_bounds = deref->arr.in_bounds;
      oi->deref.arr_index_is_const = nir_src_is_const(deref->arr.index);
      if (oi->deref.arr_index_is_const)
         oi->deref.arr_index_const = nir_src_as_int(deref->arr.index);
      else
         oi->deref.arr_index_ssa = deref->arr.index.ssa->index;
   }

   if (deref->deref_type == nir_deref_type_cast) {
      oi->deref.ptr_stride = deref->cast.ptr_stride;
      oi->deref.align_mul = deref->cast.align_mul;
      oi->deref.align_offset = deref->cast.align_offset;
   }

   if (deref->deref_type != nir_deref_type_var &&
       deref->deref_type != nir_deref_type_cast) {
      char *chain = ralloc_strdup(m->mem_ctx, "");
      append_deref_chain(m, &chain, deref, true);
      oi->deref.chain_comment = chain;
   }
}

static void
walk_intrinsic(struct own_ir_instr *oi, nir_intrinsic_instr *intr)
{
   const nir_intrinsic_info *info = &nir_intrinsic_infos[intr->intrinsic];

   oi->kind = OWN_IR_INSTR_INTRINSIC;
   oi->intrinsic.op = intr->intrinsic;
   oi->intrinsic.has_dest = info->has_dest;
   if (info->has_dest) {
      fill_value(&oi->intrinsic.dest, &intr->def);
      oi->intrinsic.dest_alu_type = nir_intrinsic_instr_dest_type(intr);
   } else {
      oi->intrinsic.dest_alu_type = nir_type_invalid;
   }
   oi->intrinsic.name = info->name;
   oi->intrinsic.num_srcs = info->num_srcs;
   oi->intrinsic.num_components = intr->num_components;
   oi->intrinsic.access =
      nir_intrinsic_has_access(intr) ? nir_intrinsic_access(intr) : 0;
   oi->intrinsic.write_mask =
      nir_intrinsic_has_write_mask(intr) ? nir_intrinsic_write_mask(intr) : 0;
   memcpy(oi->intrinsic.const_index, intr->const_index,
          sizeof(oi->intrinsic.const_index));

   unsigned n = info->num_srcs;
   if (n > OWN_IR_MAX_SRCS)
      n = OWN_IR_MAX_SRCS;
   for (unsigned i = 0; i < n; i++) {
      struct own_ir_intrinsic_src *isrc = &oi->intrinsic.src[i];
      isrc->ssa = intr->src[i].ssa->index;
      isrc->alu_type = nir_intrinsic_instr_src_type(intr, i);
      isrc->is_const = nir_src_is_const(intr->src[i]);
      if (isrc->is_const) {
         nir_load_const_instr *load = nir_def_as_load_const(intr->src[i].ssa);
         unsigned nc = load->def.num_components;
         isrc->const_components = nc;
         isrc->const_bit_size = load->def.bit_size;
         for (unsigned c = 0; c < NIR_MAX_VEC_COMPONENTS; c++)
            isrc->const_u32[c] = (c < nc) ? load->value[c].u32 : 0;
      }
   }
}

static void
walk_load_const(struct own_ir_instr *oi, nir_load_const_instr *load)
{
   oi->kind = OWN_IR_INSTR_LOAD_CONST;
   fill_value(&oi->load_const.dest, &load->def);
   for (unsigned i = 0; i < load->def.num_components && i < NIR_MAX_VEC_COMPONENTS; i++)
      oi->load_const.raw[i] = load->value[i].u32;
}

static void
walk_jump(struct own_ir_instr *oi, nir_jump_instr *jump)
{
   oi->kind = OWN_IR_INSTR_JUMP;
   oi->jump.type = jump->type;
}

static void
walk_tex(struct own_ir_instr *oi, nir_tex_instr *tex)
{
   oi->kind = OWN_IR_INSTR_TEX;
   fill_value(&oi->tex.dest, &tex->def);
   oi->tex.dest_type = tex->dest_type;
   oi->tex.op = tex->op;
   oi->tex.sampler_dim = tex->sampler_dim;
   oi->tex.is_array = tex->is_array;
   oi->tex.is_shadow = tex->is_shadow;
   oi->tex.is_sparse = tex->is_sparse;
   oi->tex.component = tex->component;
   oi->tex.is_gather_implicit_lod = tex->is_gather_implicit_lod;
   oi->tex.skip_helpers = tex->skip_helpers;
   oi->tex.texture_non_uniform = tex->texture_non_uniform;
   oi->tex.sampler_non_uniform = tex->sampler_non_uniform;
   oi->tex.texture_index = tex->texture_index;
   oi->tex.sampler_index = tex->sampler_index;
   oi->tex.need_sampler = nir_tex_instr_need_sampler(tex);
   oi->tex.has_explicit_tg4_offsets = nir_tex_instr_has_explicit_tg4_offsets(tex);
   oi->tex.backend_flags = tex->backend_flags;
   memcpy(oi->tex.tg4_offsets, tex->tg4_offsets, sizeof(oi->tex.tg4_offsets));

   unsigned n = tex->num_srcs;
   if (n > OWN_IR_MAX_SRCS)
      n = OWN_IR_MAX_SRCS;
   oi->tex.num_srcs = n;
   for (unsigned i = 0; i < n; i++) {
      struct own_ir_tex_src *ts = &oi->tex.src[i];
      ts->ssa = tex->src[i].src.ssa->index;
      ts->src_type = tex->src[i].src_type;
      ts->src_alu_type = nir_tex_instr_src_type(tex, i);
      ts->is_const = nir_src_is_const(tex->src[i].src);
      if (ts->is_const) {
         nir_load_const_instr *load =
            nir_def_as_load_const(tex->src[i].src.ssa);
         unsigned nc = load->def.num_components;
         ts->const_components = nc;
         ts->const_bit_size = load->def.bit_size;
         for (unsigned c = 0; c < NIR_MAX_VEC_COMPONENTS; c++)
            ts->const_u32[c] = (c < nc) ? load->value[c].u32 : 0;
      }
   }
}

static void
fill_const_u32(uint32_t *dst, nir_load_const_instr *load)
{
   unsigned nc = load->def.num_components;
   for (unsigned c = 0; c < NIR_MAX_VEC_COMPONENTS; c++)
      dst[c] = (c < nc) ? load->value[c].u32 : 0;
}

static void
walk_phi(struct own_ir_instr *oi, nir_phi_instr *phi, nir_block **preds,
         unsigned npreds, void *mem_ctx)
{
   oi->kind = OWN_IR_INSTR_PHI;
   fill_value(&oi->phi.dest, &phi->def);
   util_dynarray_init(&oi->phi.srcs, mem_ctx);
   for (unsigned i = 0; i < npreds; i++) {
      nir_phi_src *src = nir_phi_get_src_from_block(phi, preds[i]);
      struct own_ir_phi_src ps = { 0 };
      ps.pred = preds[i]->index;
      ps.ssa = src->src.ssa->index;
      ps.is_const = nir_src_is_const(src->src);
      if (ps.is_const) {
         nir_load_const_instr *load = nir_def_as_load_const(src->src.ssa);
         ps.const_components = load->def.num_components;
         ps.const_bit_size = load->def.bit_size;
         fill_const_u32(ps.const_u32, load);
      }
      util_dynarray_append(&oi->phi.srcs, ps);
   }
}

static void
walk_undef(struct own_ir_instr *oi, nir_undef_instr *undef)
{
   oi->kind = OWN_IR_INSTR_UNDEF;
   fill_value(&oi->undef.dest, &undef->def);
}

static void
walk_unimplemented(struct own_ir_module *m, struct own_ir_instr *oi,
                   nir_instr_type type)
{
   oi->kind = OWN_IR_INSTR_UNIMPLEMENTED;
   oi->unimplemented.type = type;
   m->unimplemented_count++;
}

static void
walk_instr(struct own_ir_module *m, struct own_ir_block *ob, nir_instr *instr,
           nir_block **preds, unsigned npreds)
{
   struct own_ir_instr *oi = own_ir_block_add_instr(ob);

   switch (instr->type) {
   case nir_instr_type_alu:
      walk_alu(oi, nir_instr_as_alu(instr));
      break;
   case nir_instr_type_deref:
      walk_deref(m, oi, nir_instr_as_deref(instr));
      break;
   case nir_instr_type_intrinsic:
      walk_intrinsic(oi, nir_instr_as_intrinsic(instr));
      break;
   case nir_instr_type_load_const:
      walk_load_const(oi, nir_instr_as_load_const(instr));
      break;
   case nir_instr_type_jump:
      walk_jump(oi, nir_instr_as_jump(instr));
      break;
   case nir_instr_type_tex:
      walk_tex(oi, nir_instr_as_tex(instr));
      break;
   case nir_instr_type_phi:
      walk_phi(oi, nir_instr_as_phi(instr), preds, npreds, m->mem_ctx);
      break;
   case nir_instr_type_undef:
      walk_undef(oi, nir_instr_as_undef(instr));
      break;
   case nir_instr_type_call:
   case nir_instr_type_cmat_call:
   default:
      walk_unimplemented(m, oi, instr->type);
      break;
   }
}

static struct own_ir_block *
walk_block(struct own_ir_module *m, nir_block *nblock, bool is_end_block)
{
   struct own_ir_block *ob = own_ir_block_create(m->mem_ctx);
   ob->index = nblock->index;
   ob->is_end_block = is_end_block;

   nir_block **preds = nir_block_get_predecessors_sorted(nblock, m->mem_ctx);
   unsigned npreds = nir_block_num_preds(nblock);
   for (unsigned i = 0; i < npreds; i++)
      own_ir_block_add_pred(ob, preds[i]->index);

   for (unsigned i = 0; i < 2; i++) {
      if (nblock->successors[i])
         own_ir_block_add_succ(ob, nblock->successors[i]->index);
   }

   nir_foreach_instr(instr, nblock)
      walk_instr(m, ob, instr, preds, npreds);

   return ob;
}

static void walk_cf_list(struct own_ir_module *m, struct exec_list *list,
                         struct util_dynarray *out);

static void
walk_if(struct own_ir_module *m, nir_if *nif, struct util_dynarray *out)
{
   struct own_ir_if *oi = own_ir_if_create(m->mem_ctx);
   oi->cond_ssa = nif->condition.ssa->index;
   oi->control = nif->control;
   oi->cond_is_const = nir_src_is_const(nif->condition);
   if (oi->cond_is_const) {
      nir_load_const_instr *load = nir_def_as_load_const(nif->condition.ssa);
      oi->const_components = load->def.num_components;
      oi->const_bit_size = load->def.bit_size;
      fill_const_u32(oi->const_u32, load);
   }
   walk_cf_list(m, &nif->then_list, &oi->then_list);
   walk_cf_list(m, &nif->else_list, &oi->else_list);
   util_dynarray_append(out, own_ir_cf_wrap_if(m->mem_ctx, oi));
}

static void
walk_loop(struct own_ir_module *m, nir_loop *loop, struct util_dynarray *out)
{
   struct own_ir_loop *ol = own_ir_loop_create(m->mem_ctx);
   ol->control = loop->control;
   ol->has_continue = nir_loop_has_continue_construct(loop);
   walk_cf_list(m, &loop->body, &ol->body);
   if (ol->has_continue)
      walk_cf_list(m, &loop->continue_list, &ol->continue_list);
   util_dynarray_append(out, own_ir_cf_wrap_loop(m->mem_ctx, ol));
}

static void
walk_cf_list(struct own_ir_module *m, struct exec_list *list,
             struct util_dynarray *out)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_block:
         util_dynarray_append(out,
            own_ir_cf_wrap_block(m->mem_ctx,
               walk_block(m, nir_cf_node_as_block(node), false)));
         break;
      case nir_cf_node_if:
         walk_if(m, nir_cf_node_as_if(node), out);
         break;
      case nir_cf_node_loop:
         walk_loop(m, nir_cf_node_as_loop(node), out);
         break;
      default:
         break;
      }
   }
}

static void
walk_function(struct own_ir_module *m, nir_function *func)
{
   struct own_ir_function *fn = own_ir_module_add_function(m);
   fn->name = func->name ? func->name : "";
   fn->is_entrypoint = func->is_entrypoint;

   if (!func->impl)
      return;

   fn->ssa_alloc = func->impl->ssa_alloc;
   nir_index_blocks(func->impl);

   {
      struct set *seen = _mesa_set_create(m->mem_ctx, _mesa_hash_string,
                                         _mesa_key_string_equal);
      unsigned name_index = 0;
      nir_foreach_function_temp_variable(var, func->impl) {
         fill_var(own_ir_function_add_local(fn), m, var, seen, &name_index);
      }
   }

   walk_cf_list(m, &func->impl->body, &fn->body);
   fn->end_block = walk_block(m, func->impl->end_block, true);
   own_ir_gather_types(fn);
}

static void
walk_variables(struct own_ir_module *m, nir_shader *nir)
{
   struct set *seen = _mesa_set_create(m->mem_ctx, _mesa_hash_string,
                                       _mesa_key_string_equal);
   unsigned name_index = 0;

   for (unsigned i = 0; i < nir_num_variable_modes; i++) {
      nir_variable_mode mode = BITFIELD_BIT(i);
      if (mode == nir_var_function_temp)
         continue;

      if (mode == nir_var_shader_in || mode == nir_var_shader_out) {
         for (unsigned loc = 0; loc < 128; loc++) {
            nir_variable *vars[NIR_MAX_VEC_COMPONENTS] = { 0 };
            nir_foreach_variable_with_modes(var, nir, mode) {
               if (var->data.location == (int)loc)
                  vars[var->data.location_frac] = var;
            }
            for (unsigned frac = 0; frac < NIR_MAX_VEC_COMPONENTS; frac++) {
               if (vars[frac])
                  walk_var(m, vars[frac], seen, &name_index);
            }
         }
      } else {
         nir_foreach_variable_with_modes(var, nir, mode)
            walk_var(m, var, seen, &name_index);
      }
   }
}

struct own_ir_module *
own_ir_walk(struct nir_shader *nir)
{
   struct own_ir_module *m = own_ir_module_create(nir);

   nir_foreach_function_impl(impl, nir)
      nir_index_ssa_defs(impl);

   m->stage = nir->info.stage;
   memcpy(m->source_blake3, nir->info.source_blake3, sizeof(m->source_blake3));
   m->name = nir->info.name;
   m->max_subgroup_size = nir->info.max_subgroup_size;
   m->min_subgroup_size = nir->info.min_subgroup_size;
     m->origin_upper_left =
        nir->info.stage == MESA_SHADER_FRAGMENT && nir->info.fs.origin_upper_left;
     m->depth_layout =
        nir->info.stage == MESA_SHADER_FRAGMENT ? nir->info.fs.depth_layout : 0;
     if (nir->info.stage == MESA_SHADER_GEOMETRY) {
        m->gs_output_primitive = nir->info.gs.output_primitive;
        m->gs_input_primitive = nir->info.gs.input_primitive;
        m->gs_vertices_out = nir->info.gs.vertices_out;
        m->gs_invocations = nir->info.gs.invocations;
        m->gs_vertices_in = nir->info.gs.vertices_in;
        m->gs_uses_end_primitive = nir->info.gs.uses_end_primitive;
        m->gs_active_stream_mask = nir->info.gs.active_stream_mask;
     }
     if (nir->info.stage == MESA_SHADER_TESS_CTRL ||
         nir->info.stage == MESA_SHADER_TESS_EVAL) {
        m->tess_primitive_mode = nir->info.tess._primitive_mode;
        m->tcs_vertices_out = nir->info.tess.tcs_vertices_out;
        m->tess_spacing = nir->info.tess.spacing;
        m->tess_ccw = nir->info.tess.ccw;
        m->tess_point_mode = nir->info.tess.point_mode;
     }
    m->workgroup_size[0] = nir->info.workgroup_size[0];
   m->workgroup_size[1] = nir->info.workgroup_size[1];
   m->workgroup_size[2] = nir->info.workgroup_size[2];
   m->workgroup_size_variable = nir->info.workgroup_size_variable;
   m->num_inputs = nir->num_inputs;
   m->num_outputs = nir->num_outputs;
   m->num_uniforms = nir->num_uniforms;
   m->num_textures = nir->info.num_textures;
   m->num_ubos = nir->info.num_ubos;
   m->num_abos = nir->info.num_abos;
   m->num_ssbos = nir->info.num_ssbos;
   m->num_images = nir->info.num_images;
   m->scratch_size = nir->scratch_size;

   walk_variables(m, nir);

   foreach_list_typed(nir_function, func, node, &nir->functions)
      walk_function(m, func);

   return m;
}

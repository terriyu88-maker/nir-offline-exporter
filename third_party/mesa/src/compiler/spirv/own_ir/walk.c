#include "ir.h"

#include <string.h>

static void
fill_value(struct own_ir_value *v, const nir_def *def)
{
   v->index = def->index;
   v->bit_size = def->bit_size;
   v->num_components = def->num_components;
}

static void
walk_var(struct own_ir_module *m, nir_variable *var)
{
   struct own_ir_var *ov = own_ir_module_add_var(m);
   ov->mode = var->data.mode;
   ov->interpolation = var->data.interpolation;
   ov->type = var->type;
   ov->name = var->name ? var->name : "";
   ov->location = var->data.location;
   ov->location_frac = var->data.location_frac;
   ov->driver_location = var->data.driver_location;
   ov->binding = var->data.binding;
   ov->access = var->data.access;

   unsigned num_components =
      glsl_get_components(glsl_without_array(var->type));
   if (num_components > 0 && num_components < 16)
      ov->component_mask = ((1u << num_components) - 1) << var->data.location_frac;
   else
      ov->component_mask = 0;
}

static void
walk_alu(struct own_ir_instr *oi, nir_alu_instr *alu)
{
   oi->kind = OWN_IR_INSTR_ALU;
   fill_value(&oi->alu.dest, &alu->def);
   oi->alu.op = alu->op;
   oi->alu.num_srcs = nir_op_infos[alu->op].num_inputs;

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
walk_deref(struct own_ir_instr *oi, nir_deref_instr *deref)
{
   oi->kind = OWN_IR_INSTR_DEREF;
   fill_value(&oi->deref.dest, &deref->def);
   oi->deref.deref_type = deref->deref_type;
   oi->deref.modes = deref->modes;
   oi->deref.type = deref->type;

   nir_variable *var = nir_deref_instr_get_variable(deref);
   if (var && var->name)
      oi->deref.var_name = var->name;
   else
      oi->deref.var_name = "";
}

static void
walk_intrinsic(struct own_ir_instr *oi, nir_intrinsic_instr *intr)
{
   const nir_intrinsic_info *info = &nir_intrinsic_infos[intr->intrinsic];

   oi->kind = OWN_IR_INSTR_INTRINSIC;
   oi->intrinsic.op = intr->intrinsic;
   oi->intrinsic.has_dest = info->has_dest;
   if (info->has_dest)
      fill_value(&oi->intrinsic.dest, &intr->def);
   oi->intrinsic.name = info->name;
   oi->intrinsic.num_srcs = info->num_srcs;
   oi->intrinsic.num_components = intr->num_components;
   oi->intrinsic.access =
      nir_intrinsic_has_access(intr) ? nir_intrinsic_access(intr) : 0;
   oi->intrinsic.write_mask =
      nir_intrinsic_has_write_mask(intr) ? nir_intrinsic_write_mask(intr) : 0;

   unsigned n = info->num_srcs;
   if (n > OWN_IR_MAX_SRCS)
      n = OWN_IR_MAX_SRCS;
   for (unsigned i = 0; i < n; i++)
      oi->intrinsic.src_ssa[i] = intr->src[i].ssa->index;
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
walk_phi(struct own_ir_instr *oi, nir_phi_instr *phi)
{
   oi->kind = OWN_IR_INSTR_PHI;
   fill_value(&oi->phi.dest, &phi->def);
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
walk_instr(struct own_ir_module *m, struct own_ir_block *ob, nir_instr *instr)
{
   struct own_ir_instr *oi = own_ir_block_add_instr(ob);

   switch (instr->type) {
   case nir_instr_type_alu:
      walk_alu(oi, nir_instr_as_alu(instr));
      break;
   case nir_instr_type_deref:
      walk_deref(oi, nir_instr_as_deref(instr));
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
      walk_phi(oi, nir_instr_as_phi(instr));
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

static void
walk_block(struct own_ir_module *m, struct own_ir_function *fn,
           nir_block *nblock, bool is_end_block)
{
   struct own_ir_block *ob = own_ir_function_add_block(fn);
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
      walk_instr(m, ob, instr);
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

   nir_foreach_block(nblock, func->impl)
      walk_block(m, fn, nblock, false);

   walk_block(m, fn, func->impl->end_block, true);
}

static void
walk_variables(struct own_ir_module *m, nir_shader *nir)
{
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
                  walk_var(m, vars[frac]);
            }
         }
      } else {
         nir_foreach_variable_with_modes(var, nir, mode)
            walk_var(m, var);
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
   m->name = nir->info.name ? nir->info.name : "";
   m->max_subgroup_size = nir->info.max_subgroup_size;
   m->min_subgroup_size = nir->info.min_subgroup_size;
   m->origin_upper_left =
      nir->info.stage == MESA_SHADER_FRAGMENT && nir->info.fs.origin_upper_left;
   m->num_inputs = nir->num_inputs;
   m->num_outputs = nir->num_outputs;
   m->num_uniforms = nir->num_uniforms;
   m->scratch_size = nir->scratch_size;

   walk_variables(m, nir);

   foreach_list_typed(nir_function, func, node, &nir->functions)
      walk_function(m, func);

   return m;
}

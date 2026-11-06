#include "ir.h"

const char *
own_ir_mesa_version(void)
{
   return "26.2.5";
}

struct own_ir_module *
own_ir_module_create(void *mem_ctx)
{
   struct own_ir_module *m = rzalloc(mem_ctx, struct own_ir_module);
   m->mem_ctx = m;
   m->stage = MESA_SHADER_NONE;
    m->name = NULL;
    m->mesa_version = own_ir_mesa_version();
    m->var_print_names = _mesa_pointer_hash_table_create(m);
    util_dynarray_init(&m->variables, m);
    util_dynarray_init(&m->functions, m);
    return m;
}

struct own_ir_var *
own_ir_module_add_var(struct own_ir_module *m)
{
   struct own_ir_var *var = rzalloc(m->mem_ctx, struct own_ir_var);
   var->name = "";
   util_dynarray_append(&m->variables, var);
   return var;
}

struct own_ir_var *
own_ir_function_add_local(struct own_ir_function *fn)
{
   struct own_ir_var *var = rzalloc(fn->mem_ctx, struct own_ir_var);
   var->name = "";
   util_dynarray_append(&fn->locals, var);
   return var;
}

struct own_ir_function *
own_ir_module_add_function(struct own_ir_module *m)
{
   struct own_ir_function *fn = rzalloc(m->mem_ctx, struct own_ir_function);
   fn->mem_ctx = m->mem_ctx;
   fn->name = "";
   util_dynarray_init(&fn->locals, m->mem_ctx);
   util_dynarray_init(&fn->body, m->mem_ctx);
   util_dynarray_append(&m->functions, fn);
   return fn;
}

struct own_ir_block *
own_ir_block_create(void *mem_ctx)
{
   struct own_ir_block *block = rzalloc(mem_ctx, struct own_ir_block);
   block->mem_ctx = mem_ctx;
   util_dynarray_init(&block->preds, mem_ctx);
   util_dynarray_init(&block->succs, mem_ctx);
   util_dynarray_init(&block->instrs, mem_ctx);
   return block;
}

struct own_ir_if *
own_ir_if_create(void *mem_ctx)
{
   struct own_ir_if *nif = rzalloc(mem_ctx, struct own_ir_if);
   util_dynarray_init(&nif->then_list, mem_ctx);
   util_dynarray_init(&nif->else_list, mem_ctx);
   return nif;
}

struct own_ir_loop *
own_ir_loop_create(void *mem_ctx)
{
   struct own_ir_loop *loop = rzalloc(mem_ctx, struct own_ir_loop);
   util_dynarray_init(&loop->body, mem_ctx);
   util_dynarray_init(&loop->continue_list, mem_ctx);
   return loop;
}

static struct own_ir_cf_node *
cf_node_create(void *mem_ctx, enum own_ir_cf_kind kind)
{
   struct own_ir_cf_node *node = rzalloc(mem_ctx, struct own_ir_cf_node);
   node->kind = kind;
   return node;
}

struct own_ir_cf_node *
own_ir_cf_wrap_block(void *mem_ctx, struct own_ir_block *block)
{
   struct own_ir_cf_node *node = cf_node_create(mem_ctx, OWN_IR_CF_BLOCK);
   node->block = block;
   return node;
}

struct own_ir_cf_node *
own_ir_cf_wrap_if(void *mem_ctx, struct own_ir_if *nif)
{
   struct own_ir_cf_node *node = cf_node_create(mem_ctx, OWN_IR_CF_IF);
   node->nif = nif;
   return node;
}

struct own_ir_cf_node *
own_ir_cf_wrap_loop(void *mem_ctx, struct own_ir_loop *loop)
{
   struct own_ir_cf_node *node = cf_node_create(mem_ctx, OWN_IR_CF_LOOP);
   node->loop = loop;
   return node;
}

struct own_ir_instr *
own_ir_block_add_instr(struct own_ir_block *block)
{
   struct own_ir_instr *instr = rzalloc(block->mem_ctx, struct own_ir_instr);
   util_dynarray_append(&block->instrs, instr);
   return instr;
}

void
own_ir_block_add_pred(struct own_ir_block *block, unsigned index)
{
   util_dynarray_append(&block->preds, index);
}

void
own_ir_block_add_succ(struct own_ir_block *block, unsigned index)
{
   util_dynarray_append(&block->succs, index);
}

static void
set_type(unsigned idx, nir_alu_type type, BITSET_WORD *float_types,
         BITSET_WORD *int_types, bool *progress)
{
   switch (nir_alu_type_get_base_type(type)) {
   case nir_type_bool:
   case nir_type_int:
   case nir_type_uint:
      if (int_types && !BITSET_TEST(int_types, idx)) {
         *progress = true;
         BITSET_SET(int_types, idx);
      }
      break;
   case nir_type_float:
      if (float_types && !BITSET_TEST(float_types, idx)) {
         *progress = true;
         BITSET_SET(float_types, idx);
      }
      break;
   default:
      break;
   }
}

static void
copy_type(unsigned src, unsigned dst, bool src_is_sink,
          BITSET_WORD *types, bool *progress)
{
   if (!types)
      return;
   if (BITSET_TEST(types, dst)) {
      if (BITSET_TEST(types, src))
         return;
      BITSET_SET(types, src);
      *progress = true;
   } else if (BITSET_TEST(types, src) && !src_is_sink) {
      BITSET_SET(types, dst);
      *progress = true;
   }
}

static void
copy_types(unsigned src, unsigned dst, bool src_is_sink,
           BITSET_WORD *float_types, BITSET_WORD *int_types, bool *progress)
{
   copy_type(src, dst, src_is_sink, float_types, progress);
   copy_type(src, dst, src_is_sink, int_types, progress);
}

static void gather_cf_list(const struct util_dynarray *list,
                           BITSET_WORD *float_types, BITSET_WORD *int_types,
                           bool *progress);

static void
gather_instr(const struct own_ir_instr *instr, BITSET_WORD *float_types,
             BITSET_WORD *int_types, bool *progress)
{
   switch (instr->kind) {
   case OWN_IR_INSTR_ALU: {
      const struct own_ir_alu *alu = &instr->alu;
      const nir_op_info *info = &nir_op_infos[alu->op];
      switch (alu->op) {
      case nir_op_mov:
      case nir_op_vec2:
      case nir_op_vec3:
      case nir_op_vec4:
      case nir_op_vec5:
      case nir_op_vec8:
      case nir_op_vec16:
         for (unsigned i = 0; i < alu->num_srcs; i++)
            copy_types(alu->src[i].ssa, alu->dest.index, alu->src[i].is_const,
                       float_types, int_types, progress);
         break;
      case nir_op_bcsel:
      case nir_op_b32csel:
         set_type(alu->src[0].ssa, nir_type_bool, float_types, int_types, progress);
         copy_types(alu->src[1].ssa, alu->dest.index, alu->src[1].is_const,
                    float_types, int_types, progress);
         copy_types(alu->src[2].ssa, alu->dest.index, alu->src[2].is_const,
                    float_types, int_types, progress);
         break;
      default:
         for (unsigned i = 0; i < alu->num_srcs; i++)
            set_type(alu->src[i].ssa, info->input_types[i],
                     float_types, int_types, progress);
         set_type(alu->dest.index, info->output_type,
                  float_types, int_types, progress);
         break;
      }
      break;
   }
   case OWN_IR_INSTR_TEX: {
      const struct own_ir_tex *tex = &instr->tex;
      for (unsigned i = 0; i < tex->num_srcs; i++)
         set_type(tex->src[i].ssa, tex->src[i].src_alu_type,
                  float_types, int_types, progress);
      set_type(tex->dest.index, tex->dest_type, float_types, int_types, progress);
      break;
   }
    case OWN_IR_INSTR_INTRINSIC: {
       const struct own_ir_intrinsic *intr = &instr->intrinsic;
       if (intr->has_dest && intr->dest_alu_type != nir_type_invalid)
          set_type(intr->dest.index, intr->dest_alu_type,
                   float_types, int_types, progress);
       for (unsigned i = 0; i < intr->num_srcs; i++) {
          if (intr->src[i].alu_type != nir_type_invalid)
             set_type(intr->src[i].ssa, intr->src[i].alu_type,
                      float_types, int_types, progress);
       }
       break;
    }
   case OWN_IR_INSTR_PHI: {
      const struct own_ir_phi *phi = &instr->phi;
      unsigned n = util_dynarray_num_elements(&phi->srcs, struct own_ir_phi_src);
      const struct own_ir_phi_src *srcs =
         n ? util_dynarray_element(&phi->srcs, struct own_ir_phi_src, 0) : NULL;
      for (unsigned i = 0; i < n; i++)
         copy_types(srcs[i].ssa, phi->dest.index, srcs[i].is_const,
                    float_types, int_types, progress);
      break;
   }
    default:
       break;
    }
}

static void
gather_block(const struct own_ir_block *block, BITSET_WORD *float_types,
             BITSET_WORD *int_types, bool *progress)
{
   util_dynarray_foreach(&block->instrs, struct own_ir_instr *, pip)
      gather_instr(*pip, float_types, int_types, progress);
}

static void
gather_cf_list(const struct util_dynarray *list, BITSET_WORD *float_types,
               BITSET_WORD *int_types, bool *progress)
{
   util_dynarray_foreach(list, struct own_ir_cf_node *, pnode) {
      struct own_ir_cf_node *node = *pnode;
      switch (node->kind) {
      case OWN_IR_CF_BLOCK:
         gather_block(node->block, float_types, int_types, progress);
         break;
      case OWN_IR_CF_IF:
         gather_cf_list(&node->nif->then_list, float_types, int_types, progress);
         gather_cf_list(&node->nif->else_list, float_types, int_types, progress);
         break;
      case OWN_IR_CF_LOOP:
         gather_cf_list(&node->loop->body, float_types, int_types, progress);
         if (node->loop->has_continue)
            gather_cf_list(&node->loop->continue_list, float_types, int_types,
                           progress);
         break;
      }
   }
}

void
own_ir_gather_types(struct own_ir_function *fn)
{
   if (!fn->ssa_alloc)
      return;
   if (!fn->float_types)
      fn->float_types = BITSET_RZALLOC(fn->mem_ctx, fn->ssa_alloc);
   if (!fn->int_types)
      fn->int_types = BITSET_RZALLOC(fn->mem_ctx, fn->ssa_alloc);

   bool progress;
   do {
      progress = false;
      gather_cf_list(&fn->body, fn->float_types, fn->int_types, &progress);
      if (fn->end_block)
         gather_block(fn->end_block, fn->float_types, fn->int_types, &progress);
   } while (progress);
}

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
   m->name = "";
   m->mesa_version = own_ir_mesa_version();
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

struct own_ir_function *
own_ir_module_add_function(struct own_ir_module *m)
{
   struct own_ir_function *fn = rzalloc(m->mem_ctx, struct own_ir_function);
   fn->mem_ctx = m->mem_ctx;
   fn->name = "";
   util_dynarray_init(&fn->blocks, m->mem_ctx);
   util_dynarray_append(&m->functions, fn);
   return fn;
}

struct own_ir_block *
own_ir_function_add_block(struct own_ir_function *fn)
{
   struct own_ir_block *block = rzalloc(fn->mem_ctx, struct own_ir_block);
   block->mem_ctx = fn->mem_ctx;
   block->index = util_dynarray_num_elements(&fn->blocks, struct own_ir_block *);
   util_dynarray_init(&block->preds, fn->mem_ctx);
   util_dynarray_init(&block->succs, fn->mem_ctx);
   util_dynarray_init(&block->instrs, fn->mem_ctx);
   util_dynarray_append(&fn->blocks, block);
   return block;
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

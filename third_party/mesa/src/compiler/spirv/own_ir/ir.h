#ifndef OWN_IR_H
#define OWN_IR_H

#include "nir.h"
#include "util/u_dynarray.h"

#include <stdio.h>

#define OWN_IR_MAX_SRCS 16

enum own_ir_emit {
   OWN_IR_EMIT_MESA = 0,
   OWN_IR_EMIT_OURS,
};

enum own_ir_instr_kind {
   OWN_IR_INSTR_ALU,
   OWN_IR_INSTR_DEREF,
   OWN_IR_INSTR_INTRINSIC,
   OWN_IR_INSTR_LOAD_CONST,
   OWN_IR_INSTR_JUMP,
   OWN_IR_INSTR_TEX,
   OWN_IR_INSTR_PHI,
   OWN_IR_INSTR_UNDEF,
   OWN_IR_INSTR_UNIMPLEMENTED,
};

struct own_ir_value {
   unsigned index;
   uint8_t bit_size;
   uint8_t num_components;
};

struct own_ir_var {
   nir_variable_mode mode;
   enum glsl_interp_mode interpolation;
   const struct glsl_type *type;
   const char *name;
   int location;
   unsigned location_frac;
   unsigned component_mask;
   unsigned driver_location;
   unsigned binding;
   unsigned access;
};

struct own_ir_alu_src {
   unsigned ssa;
   uint8_t swizzle[NIR_MAX_VEC_COMPONENTS];
   uint8_t src_components;
   bool is_const;
   uint8_t const_components;
   uint8_t const_bit_size;
   uint32_t const_u32[NIR_MAX_VEC_COMPONENTS];
};

struct own_ir_alu {
   struct own_ir_value dest;
   nir_op op;
   unsigned num_srcs;
   struct own_ir_alu_src src[NIR_ALU_MAX_INPUTS];
};

struct own_ir_deref {
   struct own_ir_value dest;
   nir_deref_type deref_type;
   const char *var_name;
   nir_variable_mode modes;
   const struct glsl_type *type;
};

struct own_ir_intrinsic {
   bool has_dest;
   struct own_ir_value dest;
   nir_intrinsic_op op;
   const char *name;
   unsigned num_srcs;
   unsigned src_ssa[OWN_IR_MAX_SRCS];
   unsigned access;
   unsigned write_mask;
   uint8_t num_components;
};

struct own_ir_load_const {
   struct own_ir_value dest;
   uint32_t raw[NIR_MAX_VEC_COMPONENTS];
};

struct own_ir_jump {
   nir_jump_type type;
};

struct own_ir_tex_src {
   unsigned ssa;
   nir_tex_src_type src_type;
   nir_alu_type src_alu_type;
   bool is_const;
   uint8_t const_components;
   uint8_t const_bit_size;
   uint32_t const_u32[NIR_MAX_VEC_COMPONENTS];
};

struct own_ir_tex {
   struct own_ir_value dest;
   nir_alu_type dest_type;
   nir_texop op;
   enum glsl_sampler_dim sampler_dim;
   unsigned num_srcs;
   struct own_ir_tex_src src[OWN_IR_MAX_SRCS];
   bool is_array;
   bool is_shadow;
   bool is_sparse;
   unsigned component;
   bool is_gather_implicit_lod;
   bool skip_helpers;
   int8_t tg4_offsets[4][2];
   bool has_explicit_tg4_offsets;
   bool texture_non_uniform;
   bool sampler_non_uniform;
   unsigned texture_index;
   unsigned sampler_index;
   bool need_sampler;
   uint32_t backend_flags;
};

struct own_ir_phi {
   struct own_ir_value dest;
};

struct own_ir_undef {
   struct own_ir_value dest;
};

struct own_ir_unimplemented {
   nir_instr_type type;
};

struct own_ir_instr {
   enum own_ir_instr_kind kind;
   union {
      struct own_ir_alu alu;
      struct own_ir_deref deref;
      struct own_ir_intrinsic intrinsic;
      struct own_ir_load_const load_const;
      struct own_ir_jump jump;
      struct own_ir_tex tex;
      struct own_ir_phi phi;
      struct own_ir_undef undef;
      struct own_ir_unimplemented unimplemented;
   };
};

struct own_ir_block {
   void *mem_ctx;
   unsigned index;
   bool is_end_block;
   struct util_dynarray preds;
   struct util_dynarray succs;
   struct util_dynarray instrs;
};

struct own_ir_function {
   void *mem_ctx;
   const char *name;
   bool is_entrypoint;
   unsigned ssa_alloc;
   struct util_dynarray blocks;
};

struct own_ir_module {
   void *mem_ctx;
   mesa_shader_stage stage;
   uint8_t source_blake3[32];
   const char *name;
   uint8_t max_subgroup_size;
   uint8_t min_subgroup_size;
   bool origin_upper_left;
   const char *mesa_version;
   unsigned num_inputs;
   unsigned num_outputs;
   unsigned num_uniforms;
   unsigned scratch_size;
   unsigned unimplemented_count;
   struct util_dynarray variables;
   struct util_dynarray functions;
};

const char *own_ir_mesa_version(void);
struct own_ir_module *own_ir_module_create(void *mem_ctx);
struct own_ir_var *own_ir_module_add_var(struct own_ir_module *m);
struct own_ir_function *own_ir_module_add_function(struct own_ir_module *m);
struct own_ir_block *own_ir_function_add_block(struct own_ir_function *fn);
struct own_ir_instr *own_ir_block_add_instr(struct own_ir_block *block);
void own_ir_block_add_pred(struct own_ir_block *block, unsigned index);
void own_ir_block_add_succ(struct own_ir_block *block, unsigned index);

struct own_ir_module *own_ir_walk(struct nir_shader *nir);
void own_ir_print_shader(struct nir_shader *nir, FILE *fp);

#endif

/*
 * Copyright © 2015 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

/*
 * A simple executable that opens a SPIR-V shader, converts it to NIR, and
 * dumps out the result.  This should be useful for testing the
 * spirv_to_nir code.
 */

#include "nir.h"
#include "nir_spirv.h"
#include "own_ir/ir.h"
#include "spirv.h"
#include "util/u_dynarray.h"
#include "vtn_private.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifndef _WIN32
#ifndef MAX_PATH
#define MAX_PATH 4096
#endif
#endif

#define WORD_SIZE 4

struct {
   const char *name;
   mesa_shader_stage stage;
} abbrev_stage_table[] = {
   { "vs",    MESA_SHADER_VERTEX },
   { "tcs",   MESA_SHADER_TESS_CTRL },
   { "tes",   MESA_SHADER_TESS_EVAL },
   { "gs",    MESA_SHADER_GEOMETRY },
   { "fs",    MESA_SHADER_FRAGMENT },
   { "cs",    MESA_SHADER_COMPUTE },
   { "cl",    MESA_SHADER_KERNEL },
   { "task",  MESA_SHADER_TASK },
   { "mesh",  MESA_SHADER_MESH },

   /* Keep previously used shader names working. */
   { "vertex",    MESA_SHADER_VERTEX },
   { "tess-ctrl", MESA_SHADER_TESS_CTRL },
   { "tess-eval", MESA_SHADER_TESS_EVAL },
   { "geometry",  MESA_SHADER_GEOMETRY },
   { "fragment",  MESA_SHADER_FRAGMENT },
   { "compute",   MESA_SHADER_COMPUTE },
   { "kernel",    MESA_SHADER_KERNEL },
};

static mesa_shader_stage
abbrev_to_stage(const char *name)
{
   for (unsigned i = 0; i < ARRAY_SIZE(abbrev_stage_table); i++) {
      if (!strcasecmp(abbrev_stage_table[i].name, name))
         return abbrev_stage_table[i].stage;
   }
   return MESA_SHADER_NONE;
}

static const char *
stage_to_abbrev(mesa_shader_stage stage)
{
   for (unsigned i = 0; i < ARRAY_SIZE(abbrev_stage_table); i++) {
      if (abbrev_stage_table[i].stage == stage)
         return abbrev_stage_table[i].name;
   }
   return "UNKNOWN";
}

static unsigned
io_type_size_vec4(const struct glsl_type *type, bool bindless)
{
   (void)bindless;
   return glsl_count_attribute_slots(type, false);
}

/* Generic NIR (L1): inline + SSA-preserving opts. No nir_lower_io, source
 * mods, nir_convert_from_ssa, or driver *_nir_lower_*.
 */
static void
run_l1_passes(nir_shader *nir)
{
   UNUSED bool _;
   bool progress;

   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_deref);

   nir_remove_non_entrypoints(nir);

   NIR_PASS(_, nir, nir_lower_variable_initializers, ~0);

   do {
      progress = false;
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_deref);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_loop);
   } while (progress);

   nir_validate_shader(nir, "after L1 passes");
}

static void
run_lower_io_contrast(nir_shader *nir)
{
   UNUSED bool _;
   NIR_PASS(_, nir, nir_lower_io,
            nir_var_shader_in | nir_var_shader_out,
            io_type_size_vec4, (nir_lower_io_options)0);
   nir_validate_shader(nir, "after nir_lower_io contrast");
}

static void
print_usage(char *exec_name, FILE *f)
{
   fprintf(f,
           "Usage: %s [options] file\n"
           "Options:\n"
           "  -h  --help              Print this help.\n"
           "  -s, --stage <stage>     Specify the shader stage.  Valid stages are:\n"
           "                          vs, tcs, tes, gs, fs, cs, cl (OpenCL-style compute),\n"
           "                          task and mesh.  Case insensitive.\n"
           "  -e, --entry <name>      Specify the entry-point name.\n"
           "  -g, --opengl            Use OpenGL environment instead of Vulkan for\n"
           "                          graphics stages.\n"
           "  --optimize              Extra NIR opts after the fixed L1 pass list.\n"
           "  --lower-io              Contrast only: run nir_lower_io on in/out after L1.\n"
            "  --emit <fmt>            Dump format: mesa (default), ours, or json.\n"
            "  --from-json             Read own_ir JSON and reprint with the own printer.\n"
            "  -o, --output <file>     Write dump to file instead of stdout.\n"
           "\n"
           "Input is SPIR-V (.spv) or GLSL (.vert/.frag/.comp/.tesc/.tese/.geom/.mesh/.task).\n"
           "GLSL is compiled with glslangValidator -V (or glslang.exe). Override\n"
           "with GLSLANG_VALIDATOR, or place the tool next to this program.\n"
           "\n"
           "L1 (always): lower temp initializers, returns, inline, copy_prop/deref,\n"
           "dce/cse, algebraic/constant_folding, dead_cf/loop. Then validate and dump.\n"
           "\n"
           "Passing the stage and the entry-point name is optional unless there's\n"
           "ambiguity, in which case the program will print the entry-points\n"
           "available.",
            exec_name);
}

static bool
has_suffix_ci(const char *path, const char *suffix)
{
   size_t n = strlen(path);
   size_t m = strlen(suffix);
   if (n < m)
      return false;
   return strcasecmp(path + n - m, suffix) == 0;
}

static mesa_shader_stage
stage_from_filename(const char *path)
{
   if (has_suffix_ci(path, ".vert"))
      return MESA_SHADER_VERTEX;
   if (has_suffix_ci(path, ".frag"))
      return MESA_SHADER_FRAGMENT;
   if (has_suffix_ci(path, ".comp"))
      return MESA_SHADER_COMPUTE;
   if (has_suffix_ci(path, ".tesc"))
      return MESA_SHADER_TESS_CTRL;
   if (has_suffix_ci(path, ".tese"))
      return MESA_SHADER_TESS_EVAL;
   if (has_suffix_ci(path, ".geom"))
      return MESA_SHADER_GEOMETRY;
   if (has_suffix_ci(path, ".mesh"))
      return MESA_SHADER_MESH;
   if (has_suffix_ci(path, ".task"))
      return MESA_SHADER_TASK;
   return MESA_SHADER_NONE;
}

static const char *
stage_to_glslang(mesa_shader_stage stage)
{
   switch (stage) {
   case MESA_SHADER_VERTEX:
      return "vert";
   case MESA_SHADER_TESS_CTRL:
      return "tesc";
   case MESA_SHADER_TESS_EVAL:
      return "tese";
   case MESA_SHADER_GEOMETRY:
      return "geom";
   case MESA_SHADER_FRAGMENT:
      return "frag";
   case MESA_SHADER_COMPUTE:
      return "comp";
   case MESA_SHADER_TASK:
      return "task";
   case MESA_SHADER_MESH:
      return "mesh";
   default:
      return NULL;
   }
}

static bool
path_is_readable(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return false;
   fclose(f);
   return true;
}

#ifdef _WIN32
static char *
find_glslang_validator(void)
{
   char buf[MAX_PATH];
   const char *env = getenv("GLSLANG_VALIDATOR");
   if (env && env[0]) {
      if (path_is_readable(env))
         return strdup(env);
      fprintf(stderr, "GLSLANG_VALIDATOR is set but not readable: %s\n", env);
      return NULL;
   }

   if (GetModuleFileNameA(NULL, buf, MAX_PATH) > 0) {
      char *slash = strrchr(buf, '\\');
      if (!slash)
         slash = strrchr(buf, '/');
      if (slash) {
         static const char *const names[] = {
            "glslangValidator.exe",
            "glslang.exe",
         };
         for (unsigned i = 0; i < ARRAY_SIZE(names); i++) {
            snprintf(slash + 1, (size_t)(MAX_PATH - (slash + 1 - buf)),
                     "%s", names[i]);
            if (path_is_readable(buf))
               return strdup(buf);
         }
      }
   }

   if (SearchPathA(NULL, "glslangValidator.exe", NULL, MAX_PATH, buf, NULL) > 0)
      return strdup(buf);
   if (SearchPathA(NULL, "glslang.exe", NULL, MAX_PATH, buf, NULL) > 0)
      return strdup(buf);
   if (SearchPathA(NULL, "glslangValidator", NULL, MAX_PATH, buf, NULL) > 0)
      return strdup(buf);
   return NULL;
}

static int
compile_glsl_to_temp_spv(const char *src, mesa_shader_stage stage,
                         char *tmp_spv, size_t tmp_spv_size)
{
   const char *glslang_stage = stage_to_glslang(stage);
   if (!glslang_stage) {
      fprintf(stderr, "GLSL stage is not supported by glslangValidator.\n");
      return 1;
   }

   char *glslang = find_glslang_validator();
   if (!glslang) {
      fprintf(stderr,
              "GLSL input requires glslangValidator.exe or glslang.exe.\n"
              "Set GLSLANG_VALIDATOR, place the tool next to spirv2nir.exe, "
              "or install the Vulkan SDK.\n");
      return 1;
   }

   char tmp_dir[MAX_PATH];
   DWORD n = GetTempPathA(MAX_PATH, tmp_dir);
   if (n == 0 || n >= MAX_PATH) {
      fprintf(stderr, "Failed to get temp path\n");
      free(glslang);
      return 1;
   }

   if (!GetTempFileNameA(tmp_dir, "spv", 0, tmp_spv)) {
      fprintf(stderr, "Failed to create temp SPIR-V file\n");
      free(glslang);
      return 1;
   }
   (void)tmp_spv_size;

   char cmd[4096];
   int printed = snprintf(cmd, sizeof(cmd),
                          "\"%s\" -V -S %s -o \"%s\" \"%s\"",
                          glslang, glslang_stage, tmp_spv, src);
   if (printed < 0 || (size_t)printed >= sizeof(cmd)) {
      fprintf(stderr, "glslangValidator command line is too long\n");
      DeleteFileA(tmp_spv);
      free(glslang);
      return 1;
   }

   STARTUPINFOA si;
   PROCESS_INFORMATION pi;
   memset(&si, 0, sizeof(si));
   memset(&pi, 0, sizeof(pi));
   si.cb = sizeof(si);

   if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
      fprintf(stderr, "Failed to launch %s (error %lu)\n",
              glslang, (unsigned long)GetLastError());
      DeleteFileA(tmp_spv);
      free(glslang);
      return 1;
   }

   WaitForSingleObject(pi.hProcess, INFINITE);
   DWORD code = 1;
   GetExitCodeProcess(pi.hProcess, &code);
   CloseHandle(pi.hProcess);
   CloseHandle(pi.hThread);
   free(glslang);

   if (code != 0) {
      fprintf(stderr, "glslangValidator failed with exit code %lu\n",
              (unsigned long)code);
      DeleteFileA(tmp_spv);
      return 1;
   }

   return 0;
}
#endif

static void
maybe_delete_temp_spv(const char *path, bool do_delete)
{
#ifdef _WIN32
   if (do_delete && path && path[0])
      DeleteFileA(path);
#else
   (void)path;
   (void)do_delete;
#endif
}

struct entry_point {
   const char *name;
   mesa_shader_stage stage;
};

static struct entry_point
select_entry_point(void *mem_ctx, const uint32_t *words, size_t word_count,
                  struct entry_point args)
{
   /* Create a dummy vtn_builder to use with vtn_string_literal. */
   struct vtn_builder *b = rzalloc(mem_ctx, struct vtn_builder);

   struct util_dynarray candidates;
   util_dynarray_init(&candidates, mem_ctx);

   /* Skip header. */
   const uint32_t *w = words + 5;
   const uint32_t *end = words + word_count;

   bool seen_entry_point = false;
   while (w < end) {
      SpvOp opcode = w[0] & SpvOpCodeMask;
      unsigned count = w[0] >> SpvWordCountShift;
      assert(count >= 1 && w + count <= end);

      if (opcode == SpvOpEntryPoint) {
         seen_entry_point = true;

         unsigned name_words;
         const char *name = vtn_string_literal(b, &w[3], count - 3, &name_words);
         mesa_shader_stage stage = vtn_stage_for_execution_model(w[1]);

         struct entry_point e = { name, stage };
         util_dynarray_append(&candidates, e);
      } else if (seen_entry_point) {
         /* List of entry_points is over, we can break now. */
         break;
      }

      w += count;
   }

   struct entry_point r = {0};
   if (util_dynarray_num_elements(&candidates, struct entry_point) == 0) {
      fprintf(stderr, "ERROR: No entry-points available.\n");
      return r;
   }

   int matches = 0;
   util_dynarray_foreach(&candidates, struct entry_point, e) {
      if ((!args.name || !strcmp(args.name, e->name)) &&
          (args.stage == MESA_SHADER_NONE || args.stage == e->stage)) {
         if (matches == 0) {
            /* Save the first match we found. */
            r = *e;
         }
         matches++;
      }
   }

   if (matches != 1) {
      if (matches == 0) {
         fprintf(stderr, "No matching entry-point for arguments passed.\n");
      } else {
         fprintf(stderr, "Multiple entry-points available, select with --stage and/or --entry.\n");

         /* Discard whatever we found before. */
         r.name = NULL;
         r.stage = MESA_SHADER_NONE;
      }

      fprintf(stderr, "Entry-points available:\n");
      util_dynarray_foreach(&candidates, struct entry_point, e)
         fprintf(stderr, "  --entry e \"%s\" --stage %s\n", e->name, stage_to_abbrev(e->stage));
   }

   return r;
}

int main(int argc, char **argv)
{
   struct entry_point entry_point = {
      .name = NULL,
      .stage = MESA_SHADER_NONE,
   };
   int ch;
    bool optimize = false;
    bool lower_io = false;
    bool from_json = false;
    enum own_ir_emit emit = OWN_IR_EMIT_MESA;
    enum nir_spirv_execution_environment env = NIR_SPIRV_VULKAN;
    const char *out_path = NULL;

   static struct option long_options[] =
      {
         {"help",     no_argument,       0, 'h'},
         {"stage",    required_argument, 0, 's'},
         {"entry",    required_argument, 0, 'e'},
         {"opengl",   no_argument,       0, 'g'},
         {"optimize", no_argument,       0, 'O'},
          {"lower-io", no_argument,       0, 'L'},
          {"emit",     required_argument, 0, 'E'},
          {"from-json", no_argument,      0, 'J'},
          {"output",   required_argument, 0, 'o'},
          {0, 0,                          0, 0}
       };

    while ((ch = getopt_long(argc, argv, "hs:e:go:", long_options, NULL)) != -1) {
      switch (ch) {
      case 'h':
         print_usage(argv[0], stdout);
         return 0;
      case 's': {
         mesa_shader_stage s = abbrev_to_stage(optarg);
         if (s == MESA_SHADER_NONE) {
            fprintf(stderr, "Unknown stage \"%s\"\n", optarg);
            print_usage(argv[0], stderr);
            return 1;
         }
         entry_point.stage = s;
         break;
      }
      case 'e':
         entry_point.name = optarg;
         break;
      case 'g':
         env = NIR_SPIRV_OPENGL;
         break;
      case 'O':
         optimize = true;
         break;
      case 'L':
         lower_io = true;
         break;
      case 'E':
         if (!strcmp(optarg, "mesa")) {
            emit = OWN_IR_EMIT_MESA;
         } else if (!strcmp(optarg, "ours")) {
            emit = OWN_IR_EMIT_OURS;
         } else if (!strcmp(optarg, "json")) {
            emit = OWN_IR_EMIT_JSON;
         } else {
            fprintf(stderr, "Unknown --emit \"%s\" (mesa|ours|json)\n", optarg);
            print_usage(argv[0], stderr);
            return 1;
         }
         break;
      case 'J':
         from_json = true;
         break;
      case 'o':
         out_path = optarg;
         break;
      default:
         fprintf(stderr, "Unrecognized option \"%s\".\n", optarg);
         print_usage(argv[0], stderr);
         return 1;
      }
   }

    const char *filename = argv[optind];
    if (!filename) {
       fprintf(stderr, "Missing input file.\n");
       print_usage(argv[0], stderr);
       return 1;
    }

    if (from_json) {
       if (!filename) {
          fprintf(stderr, "Missing JSON file.\n");
          print_usage(argv[0], stderr);
          return 1;
       }
       FILE *out = stdout;
       if (out_path) {
          out = fopen(out_path, "w");
          if (!out) {
             fprintf(stderr, "Failed to open %s for writing\n", out_path);
             return 1;
          }
       }
       char *err = NULL;
       struct own_ir_module *m = own_ir_from_json(NULL, filename, &err);
       if (!m) {
          fprintf(stderr, "Failed to read JSON %s: %s\n", filename,
                  err ? err : "unknown error");
          if (out != stdout)
             fclose(out);
          return 1;
       }
       own_ir_print_module(m, out);
       if (out != stdout)
          fclose(out);
       ralloc_free(m);
       return 0;
    }

    char compiled_spv[MAX_PATH];
    compiled_spv[0] = '\0';
    bool delete_compiled_spv = false;
    if (!has_suffix_ci(filename, ".spv") &&
        !has_suffix_ci(filename, ".json")) {
       mesa_shader_stage from_ext = stage_from_filename(filename);
       if (from_ext == MESA_SHADER_NONE) {
          fprintf(stderr,
                  "Unknown input \"%s\". Use .spv or GLSL "
                  "(.vert/.frag/.comp/.tesc/.tese/.geom/.mesh/.task).\n",
                  filename);
          return 1;
       }
       if (entry_point.stage == MESA_SHADER_NONE)
          entry_point.stage = from_ext;
#ifndef _WIN32
       fprintf(stderr,
               "GLSL input is only compiled on Windows. Pass a .spv file "
               "or run glslangValidator -V first.\n");
       return 1;
#else
       if (compile_glsl_to_temp_spv(filename, entry_point.stage,
                                    compiled_spv, sizeof(compiled_spv)))
          return 1;
       filename = compiled_spv;
       delete_compiled_spv = true;
#endif
    }

    FILE *f = fopen(filename, "rb");
   if (!f) {
      fprintf(stderr, "Failed to open %s\n", filename);
      maybe_delete_temp_spv(compiled_spv, delete_compiled_spv);
      return 1;
   }

   if (fseek(f, 0, SEEK_END) != 0) {
      fprintf(stderr, "Failed to seek %s\n", filename);
      fclose(f);
      maybe_delete_temp_spv(compiled_spv, delete_compiled_spv);
      return 1;
   }

   long len = ftell(f);
   if (len < (long)WORD_SIZE || len % WORD_SIZE != 0) {
      fprintf(stderr, "File length isn't a multiple of the word size\n");
      fprintf(stderr, "Are you sure this is a valid SPIR-V shader?\n");
      fclose(f);
      maybe_delete_temp_spv(compiled_spv, delete_compiled_spv);
      return 1;
   }

   size_t word_count = (size_t)len / WORD_SIZE;
   void *map = malloc((size_t)len);
   if (!map) {
      fprintf(stderr, "Failed to allocate %ld bytes\n", len);
      fclose(f);
      maybe_delete_temp_spv(compiled_spv, delete_compiled_spv);
      return 1;
   }

   rewind(f);
   if (fread(map, 1, (size_t)len, f) != (size_t)len) {
      fprintf(stderr, "Failed to read %s\n", filename);
      free(map);
      fclose(f);
      maybe_delete_temp_spv(compiled_spv, delete_compiled_spv);
      return 1;
   }
   fclose(f);

   const uint32_t *words = map;
   if (words[0] != SpvMagicNumber) {
      fprintf(stderr, "ERROR: Not a SPIR-V file. First word was 0x%x, want 0x%x (SPIR-V magic).",
              words[0], SpvMagicNumber);
      free(map);
      maybe_delete_temp_spv(compiled_spv, delete_compiled_spv);
      return 1;
   }

   void *mem_ctx = ralloc_context(NULL);

   entry_point = select_entry_point(mem_ctx, map, word_count, entry_point);
   if (!entry_point.name) {
      ralloc_free(mem_ctx);
      free(map);
      maybe_delete_temp_spv(compiled_spv, delete_compiled_spv);
      return 1;
   }

   glsl_type_singleton_init_or_ref();

   struct nir_shader_compiler_options nir_opts = {0};

   struct spirv_to_nir_options spirv_opts = {
      .environment = env,
   };

   if (entry_point.stage == MESA_SHADER_KERNEL)
      spirv_opts.environment = NIR_SPIRV_OPENCL;

   nir_shader *nir = spirv_to_nir(map, word_count, NULL,
                                  entry_point.stage, entry_point.name,
                                  &spirv_opts, &nir_opts);

   if (nir) {
      run_l1_passes(nir);

      if (optimize) {
         bool progress;
         do {
            progress = false;

            #define OPT(pass, ...) ({                                  \
               bool this_progress = false;                             \
               NIR_PASS(this_progress, nir, pass, ##__VA_ARGS__);      \
               if (this_progress)                                      \
                  progress = true;                                     \
               this_progress;                                          \
            })

            OPT(nir_opt_dce);
            OPT(nir_opt_cse);
            OPT(nir_opt_dead_cf);
            OPT(nir_lower_vars_to_ssa);
            OPT(nir_opt_copy_prop);
            OPT(nir_opt_deref);
            OPT(nir_opt_constant_folding);
            OPT(nir_opt_copy_prop_vars);
            OPT(nir_opt_dead_write_vars);
            OPT(nir_opt_combine_stores, nir_var_all);
            OPT(nir_remove_dead_variables, nir_var_function_temp, NULL);
            OPT(nir_opt_algebraic);
            OPT(nir_opt_if, 0);
            OPT(nir_opt_loop_unroll);

            #undef OPT
         } while (progress);

         nir_validate_shader(nir, "after --optimize");
      }

      if (lower_io)
         run_lower_io_contrast(nir);

      FILE *out = stdout;
      if (out_path) {
         out = fopen(out_path, "w");
          if (!out) {
             fprintf(stderr, "Failed to open %s for writing\n", out_path);
             glsl_type_singleton_decref();
             ralloc_free(mem_ctx);
             free(map);
             maybe_delete_temp_spv(compiled_spv, delete_compiled_spv);
             return 1;
          }
      }

      const char *api = "vulkan";
      if (entry_point.stage == MESA_SHADER_KERNEL || env == NIR_SPIRV_OPENCL)
         api = "opencl";
      else if (env == NIR_SPIRV_OPENGL)
         api = "opengl";

      if (emit == OWN_IR_EMIT_OURS)
         own_ir_print_shader(nir, out);
      else if (emit == OWN_IR_EMIT_JSON)
         own_ir_print_json(nir, out, api);
      else
         nir_print_shader(nir, out);

      if (out != stdout)
         fclose(out);
   } else {
      fprintf(stderr, "SPIRV to NIR compilation failed\n");
   }

    glsl_type_singleton_decref();

    ralloc_free(mem_ctx);
    free(map);
    maybe_delete_temp_spv(compiled_spv, delete_compiled_spv);

    return 0;
}

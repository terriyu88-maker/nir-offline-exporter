# Mesa NIR 离线编译器：自有 IR 导出计划书

## 1. 目标

在 Windows 上交付一个离线工具：接受 SPIR-V（GLSL 经 glslang 转为 SPIR-V 后进入同一路径），调用 Mesa 的 `spirv_to_nir()`，在硬件无关层（generic NIR / L1）立刻遍历内存中的 `nir_shader`，映射到自有 IR（进程内内存形式）。JSON 仅为对照导出，不实现自有二进制存储格式。

**自有 IR 的主验收目标：** 对自有 IR 再打印一遍，文本与 `nir_print_shader()` 的 NIR dump 一致。打印内容对齐即视为映射做对。

```text
.vert/.frag/.comp  --glslangValidator-->  .spv
.spv               --spirv_to_nir()-->  nir_shader (L1, 硬件无关)
                         |
                         +--> nir_print_shader()              --> a.nir.txt
                         |
                          +--> 遍历 --> 自有 IR（内存）--> 自有打印机 --> b.nir.txt
                          |                 |
                          |                 +--> JSON（对照）
                          |
                         diff a.nir.txt b.nir.txt  == 空
```

不解析 `nir_serialize()` blob。遍历入口永远是内存里的 `nir_shader`。

## 2. 总约束

- 停在 generic NIR（L1）：inline、SSA、通用 opt；不做 `nir_lower_io`、source mods、`nir_convert_from_ssa`、任何驱动 `*_nir_lower_*`。
- `nir_shader_compiler_options` 按能力全集填写，硬件能做的 opcode 保留，不提前拆。
- 钉死 Mesa git tag、shader stage、Vulkan 语义。
- 打印机按 NIR 的遍历顺序走：函数声明序 → 块列表序 → 指令列表序。SSA id 与 `nir_print_shader` 同一套编号规则。
- 每个阶段有独立可运行产物，过验收再进下一阶段。
- 编译环境：Linux 上 MinGW 交叉编译，产出 Windows PE（`spirv2nir.exe`）。本机不搭 VS。阶段 0–3 用 Wine 冒烟；干净 Win10/11 实机验收放到阶段 4。

## 3. 分阶段实施

### 阶段 0：Linux MinGW 交叉编译出 Windows exe

**做：**

- 在 Linux 安装：meson、ninja、python3-mako、python3-pyyaml、bison、flex、mingw-w64（`x86_64-w64-mingw32-gcc/g++`）、wine
- clone Mesa 并钉死 git tag
- 写 meson cross file（`mingw-w64.ini`），按 Mesa 官方 Windows 交叉示例：
  - `c` / `cpp` / `ar` / `strip` / `pkg-config` 指向 `x86_64-w64-mingw32-*`
  - `exe_wrapper = 'wine'`
  - `host_machine.system = 'windows'`，`cpu_family = 'x86_64'`
- meson 只开 compiler + `tools=nir`，关闭 LLVM / Vulkan ICD / D3D12 / 完整驱动

建议配置：

```bash
meson setup build-mingw \
  --cross-file mingw-w64.ini \
  --backend=ninja \
  -Dbuildtype=release \
  -Dplatforms=windows \
  -Dgallium-drivers=[] \
  -Dvulkan-drivers=[] \
  -Dllvm=disabled \
  -Dopengl=disabled \
  -Degl=disabled \
  -Dgbm=disabled \
  -Dtools=nir
```

若 `gallium-drivers=[]` 配不过，改 `-Dgallium-drivers=softpipe`，只编 compiler 目标：

```bash
ninja -C build-mingw src/compiler/spirv/spirv2nir.exe
```

**产物：** Linux 上交叉编出的 `spirv2nir.exe`（Windows PE）

**验收：**

- `file spirv2nir.exe` 显示 PE32+ x86-64
- `wine spirv2nir.exe --help` 退出码 0
- Wine 下喂最小 Vulkan fragment `.spv`，stdout 有 NIR 文本
- 不依赖 LLVM / Vulkan ICD / D3D12
- 本机无需 Visual Studio / WinFlexBison

### 阶段 1：固定硬件无关 pass，并 dump NIR 文本

**做：**

- 在 `spirv_to_nir()` 之后插入固定 pass 列表：
  - `nir_lower_variable_initializers`
  - `nir_lower_returns`
  - `nir_inline_functions`
  - `nir_copy_prop` / `nir_opt_deref`
  - `nir_opt_dce` / `nir_opt_cse`
  - `nir_opt_algebraic` / `nir_opt_constant_folding`
  - `nir_opt_dead_cf` / `nir_opt_loop`
- `nir_validate_shader()` 通过后再 `nir_print_shader()`
- 明确禁止：`nir_lower_io`、`nir_lower_to_source_mods`、`nir_convert_from_ssa`、各驱动 `*_nir_lower_*`

**产物：** `shader.nir.txt`（Mesa 官方打印机输出，后续所有对照的基准）

**验收：**

- 同一输入两次 dump 文本一致
- dump 里能看到 `fadd` / `fmul` 等 ALU，看不到驱动后端指令
- 故意打开 `nir_lower_io` 的对照构建，与默认构建可 diff 出 IO lowering 差异
- `nir_validate_shader()` 通过

### 阶段 2：遍历器 + 内存中的自有 IR + 自有打印机

**做：**

- 用 `nir_foreach_function_impl` / `nir_foreach_block` / `nir_foreach_instr` 遍历
- 映射到自有结构：
  - Module：stage、entrypoint、Mesa 版本
  - Function → Block（CFG 前驱/后继）
  - Instr：Alu / Tex / Intrinsic / Phi / Jump / Undef / LoadConst
  - Value：SSA id、bit size、num components
  - Variable：in / out / ubo / ssbo 元数据
- 实现自有打印机，输出格式对齐 `nir_print_shader()`
- 先打通 ALU + const + jump；其余类型记 `Unimplemented` 并计数。某类指令一旦出现在黄金件中，该类必须能打印到与 NIR 一致，才算该类完成

**产物：** 进程内自有 IR；`b.nir.txt`（自有打印机输出）

**完成定义：** 自有 IR 能打印出与 NIR dump 相同的文本。

**验收：**

- 黄金件 1（纯 ALU FS）：`diff a.nir.txt b.nir.txt` 为空
- 打印内容对齐：函数、块 label、SSA 编号、opcode 名、src/dst、swizzle、类型宽度
- `nir_validate_shader` 通过
- 未覆盖指令类型允许白名单，白名单必须写进报告；黄金件覆盖到的类型白名单必须清零

**落点：** 自有代码放在 `third_party/mesa/src/compiler/spirv/own_ir/`，编进同一 `spirv2nir.exe`。调用点只改 `spirv2nir.c`。

**对照物：** 黄金件 1 是 `shaders/min.frag` 经 L1 后的 dump（`out/min.frag.nir.txt`）。当前指令：`deref_var`、`@load_deref`、`load_const`、`fmul`、`@store_deref`。`nir_lower_returns` 之后这块没有 `return`。

**CLI：** `spirv2nir shader.spv` 仍是 Mesa 文本；`spirv2nir shader.spv --emit ours` 是自有打印机。阶段 3 的 json 接到同一个 `--emit`。

先把黄金件 1 打到 `diff` 为空，再按黄金件 2–5 补 Tex / UBO / if-loop / SSBO。

#### 阶段 2 细化任务

- [x] 1. 落点与构建
  - [x] 1.1 新建 `own_ir/`：`ir.h` `ir.c` `walk.c` `print.c`
  - [x] 1.2 `meson.build` 把上述文件加入 `spirv2nir` 可执行文件源
  - [x] 1.3 `spirv2nir.c` 增加 `--emit ours`（默认仍走 Mesa 打印机）
  - [x] 1.4 MinGW 重编，Wine `--help` 能看到该选项

- [x] 2. 自有 IR 结构（只建模 dump 用到的字段）
  - [x] 2.1 Module：stage、blake3、name、subgroup 范围、`origin_upper_left`、Mesa `26.2.5`
  - [x] 2.2 Variable：mode、interp、type、name、location、location_frac、component mask
  - [x] 2.3 Function / Block：名字、entrypoint、preds/succs、指令列表
  - [x] 2.4 Value：SSA id、bit_size、num_components
  - [x] 2.5 Instr：Alu / Deref / Intrinsic / LoadConst / Jump / Unimplemented（Tex / Phi / Undef 先占位）

- [x] 3. 遍历器骨架
  - [x] 3.1 L1 + `nir_validate_shader` 之后、`nir_print_shader` 之前调用 `nir_index_ssa_defs`（与 Mesa 打印机同一套 `%id`）
  - [x] 3.2 按 `nir_print.c` 顺序走：shader info → 变量（in/out 按 location 0–127，再 `location_frac`）→ `foreach_list` 函数 → 块 → 指令
  - [x] 3.3 未实现的 `nir_instr_type` 记 Unimplemented 并计数，打印一行可检索标记
  - [x] 3.4 黄金件 1 上 Unimplemented 计数必须为 0

- [x] 4. 打印机：shader 头
  - [x] 4.1 对齐 `print_shader_info`：`shader:` / `source_blake3:` / `name:` / subgroup / `origin_upper_left`
  - [x] 4.2 `inputs`/`outputs`/`uniforms`/`scratch` 仅非零才打印
  - [x] 4.3 Wine：头与 Mesa dump 一致（可先忽略变量和指令体）

- [x] 5. 打印机：变量与函数声明
  - [x] 5.1 `decl_var` 对齐黄金件 1 两行 in/out（含 `INTERP_MODE_NONE`、`VARYING_SLOT_VAR0`、`FRAG_RESULT_DATA0`、`.xyzw`）
  - [x] 5.2 `decl_function main () (entrypoint)` 后空行，再 `impl main {`

- [x] 6. 打印机：CFG
  - [x] 6.1 `block bN:`、`// preds:`、块尾 `// succs: bM`
  - [x] 6.2 黄金件 1：`b0` 无前驱、后继 `b1`；空块 `b1`

- [x] 7. 指令：LoadConst / Deref / ALU / Intrinsic
  - [x] 7.1 `load_const`：hex + 十进制内联，格式对齐 `%2` 那行
  - [x] 7.2 `deref_var &name (shader_in/out type)`，含 bit 宽前缀 `32    %0`
  - [x] 7.3 ALU：`fmul`、src SSA、常量源内联 `(0.500000, ...)`、`32x4` 前缀
  - [x] 7.4 `@load_deref` / `@store_deref`：`access=none`、`wrmask=xyzw`；store 无 SSA dest
  - [x] 7.5 缩进、空格、空行与 `nir_print.c` 逐字符对齐

- [x] 8. Jump（占位，黄金件 1 可不出现）
  - [x] 8.1 映射 `nir_jump_return` 等，打印 `return`
  - [x] 8.2 黄金件 1 当前无 jump，有则必须与 Mesa 一致

- [x] 9. 检查点：黄金件 1
  - [x] 9.1 Wine 两次：默认 dump → `a.nir.txt`；`--emit ours` → `b.nir.txt`
  - [x] 9.2 `diff -u a.nir.txt b.nir.txt` 为空
  - [x] 9.3 报告：黄金件 1 覆盖的类型白名单清零；Tex/Phi/Undef 可留白名单

- [x] 10. 检查点：黄金件 2（纹理采样 FS）
  - [x] 10.1 `shaders/tex.frag`：`texture(sampler2D, uv)` 经 `glslangValidator -V`
  - [x] 10.2 walker 填 `nir_tex_instr`：op、dest_type、src（ssa + `src_type`）、sampler_dim、index/non-uniform/tg4
  - [x] 10.3 打印机对齐 `print_tex_instr`：`(float32)tex %0 (texture_deref), %0 (sampler_deref), %2 (coord), 2D`
  - [x] 10.4 `decl_var` access 按 Mesa 位名打印（黄金件 2 的 `restrict`）
  - [x] 10.5 Wine：`tex.frag.spv` 与 `min.frag.spv` 的默认 dump vs `--emit ours`，`diff` 均为空

- [x] 11. 检查点：黄金件 3（带 UBO 的 VS）
  - [x] 11.1 `shaders/ubo.vert`：`uniform UBO { mat4 mvp; }` 经 `glslangValidator -V`
  - [x] 11.2 shader 头：`num_ubos` 在 subgroup 之前打印
  - [x] 11.3 匿名 `gl_PerVertex` 打成 `#0`；VS in 用 `VERT_ATTRIB_GENERIC0`
  - [x] 11.4 deref：`deref_cast` / `deref_struct` / `deref_array`（含 `(in bounds)` 与整链注释）
  - [x] 11.5 intrinsic：`@vulkan_resource_index` / `@load_vulkan_descriptor` 的 `desc_set`/`binding`/`desc_type`/`resource_type`；const src 内联 `(0x0)`
  - [x] 11.6 ALU swizzle：`%17.wwww` 等与 Mesa 一致
  - [x] 11.7 Wine：`ubo.vert.spv` 以及黄金件 1–2 回归，`diff` 均为空

- [x] 12. 检查点：黄金件 4（带分支/循环的 FS）
  - [x] 12.1 `shaders/ifloop.frag`：if/else + `for` 经 `glslangValidator -V`
  - [x] 12.2 walker 按 CF 树走 `nir_cf_node`：block / if / loop，不再线性扫块
  - [x] 12.3 打印机对齐 `print_if` / `print_loop`：嵌套缩进、`} else {`、空块 preds/succs 同行
  - [x] 12.4 function_temp 在 `impl` 内打印（`c`/`n`/`i`/`loop_break`/`loop_continue`）
  - [x] 12.5 ALU：`fp_math_ctrl` 打 `// preserve:inf,nan`；整数常量按 uint 内联
  - [x] 12.6 phi 源按 sorted preds 填（本黄金件无 phi）
  - [x] 12.7 Wine：`ifloop.frag.spv` 以及黄金件 1–3 回归，`diff` 均为空

- [x] 13. 检查点：黄金件 5（带 SSBO/原子的 CS）
  - [x] 13.1 `shaders/ssbo.comp`：`atomicAdd` + SSBO 数组写，经 `glslangValidator -V`
  - [x] 13.2 shader 头：`workgroup_size` 在 `num_ssbos` 之前打印
  - [x] 13.3 system value：`gl_GlobalInvocationID` 打成 `(SYSTEM_VALUE_GLOBAL_INVOCATION_ID)`（无 driver_location）
  - [x] 13.4 `@deref_atomic`：`access=coherent`、`atomic_op=iadd`
  - [x] 13.5 Wine：`ssbo.comp.spv` 以及黄金件 1–4 回归，`diff` 均为空

- [x] 14. 检查点：黄金件 6（带 `phi` 的合流 FS）
  - [x] 14.1 `shaders/phi.frag.spvasm`：手写 `OpPhi`；默认 L1 会把 phi 降成 function_temp，对照用 `--optimize`
  - [x] 14.2 空 `name`：`nir->info.name == NULL` 不打印 `name:`；空串 `""` 仍打印 `name: `（与 Mesa 一致）
  - [x] 14.3 ALU：`vecN` 源通道按 `nir_op_infos[].input_sizes`（`nir_ssa_alu_instr_src_components`），标量进 `vec4` 不打 `.xyzw`
  - [x] 14.4 JSON：`name` 缺省表示 NULL；读回后打印机与 dump 一致
  - [x] 14.5 Wine：`--optimize` 下 mesa vs ours vs `--from-json`，`diff` 为空；黄金件 1–5 回归

### 阶段 3：JSON 导出（对照格式）

**做：**

- 把自有 IR 写成 JSON：SSA 用整数 id，opcode 用 Mesa 枚举名字符串，块用 label
- CLI：`spirv2nir shader.spv --emit json -o shader.json`
- 文件头含 `mesa_git`、`stage`、`api=vulkan`

**产物：** `shader.json`

**验收：**

- JSON schema 校验通过
- `JSON → 自有 IR → 自有打印机` 的文本与原始 NIR dump 一致（`diff` 为空）
- 黄金件（至少 VS 变换、FS 贴图、CS 原子）JSON 纳入 git，回归 diff 为空

- [x] 1. Schema + 写出（当前步）
  - [x] 1.1 `own_ir.schema.json`：SSA 整数、opcode/mode 用 Mesa 枚举名、块 `bN`、CF 嵌套、GLSL 类型名
  - [x] 1.2 `own_ir/json.c`：自有 IR → JSON；头含 `mesa_git` / `stage` / `api`
  - [x] 1.3 CLI：`--emit json`、`-o`；Wine 对黄金件 1–5 写出可解析 JSON
- [x] 2. JSON 读回 → 自有 IR → 自有打印机，与 dump `diff` 空
- [x] 3. 黄金件 JSON 入库

### 阶段 4：GLSL 入口与 Windows 分发

**做：**

- 输入 `.vert/.frag/.comp` 时调用 `glslangValidator -V`，再走同一条 NIR 路径
- `.spv` 直接走
- 静态链 MinGW 运行时（`-static` 或带上 `libgcc_s_seh-1.dll` / `libwinpthread-1.dll` / `libstdc++-6.dll`）
- 打包 zip：`spirv2nir.exe` + 必要 MinGW DLL + `glslangValidator.exe`（或写明需 Vulkan SDK）
- CLI 支持 `--emit mesa|ours|json`
- 阶段 0–3 继续用 Wine 回归；本阶段用一台干净 Win10/11 或 CI `windows-latest` 做实机验收

**产物：** Windows 分发包

**验收：**

- 同一份 GLSL：`glslang → spirv2nir` 的 NIR dump 与自有 IR 打印文本一致
- Wine 与干净 Win10/11（无 Mesa 源码、无 VS）都能跑黄金件
- `--emit mesa|ours|json` 三种输出都可用
- `ldd`/`objdump -p` 确认未链 Linux 系统库

#### 阶段 4 细化任务

- [x] 1. GLSL 入口
  - [x] 1.1 按扩展名分流：`.spv` 直接读；`.vert/.frag/.comp/.tesc/.tese/.geom/.mesh/.task` 先交给 glslang
  - [x] 1.2 未给 `--stage` 时从扩展名推断 Mesa stage 与 glslang `-S`
  - [x] 1.3 查找 `glslangValidator.exe` / `glslang.exe`：环境变量 `GLSLANG_VALIDATOR`、与 `spirv2nir.exe` 同目录、再 PATH
  - [x] 1.4 Windows `CreateProcess`：`glslangValidator -V -S <stage> -o <temp.spv> <file>`，失败时打印退出码
  - [x] 1.5 临时 `.spv` 走现有 `spirv_to_nir` + L1，用完删除
  - [x] 1.6 Wine：黄金件 `.spv` 回归；无 glslang 时 `.frag` 报错清晰；有 `glslang.exe` 时同一份 GLSL 与对应 `.spv` 的 dump 一致（`source_blake3` 因 SPIR-V 字节不同可不同）

- [x] 2. 检查点：`--emit mesa|ours|json`
  - [x] 2.1 Wine 黄金件 1–5：三种 emit 仍可用
  - [x] 2.2 `--emit ours` vs Mesa dump `diff` 空

- [x] 3. MinGW 运行时 DLL
  - [x] 3.1 `objdump -p` 列出 PE 依赖
  - [x] 3.2 随包复制所需 DLL（当前有 `libwinpthread-1.dll`）；确认无 Linux 系统库

- [x] 4. 分发包与文档
  - [x] 4.1 `scripts/pack-windows.sh` 产出 `dist/spirv2nir/`：exe + DLL，并写明需 Vulkan SDK / `glslangValidator.exe`
  - [x] 4.2 README 更新 GLSL 用法与分发说明

## 4. 黄金件与回归

最少 5 个输入，全部同时出 `nir.txt`（Mesa 打印）和 `ours.txt`（自有 IR 打印），CI 对这两份做 diff。JSON 由内存中的自有 IR 再导出，读回后仍须打出与 `nir.txt` 相同的文本。

1. 纯 ALU fragment（`color * 0.5` 的 Vulkan 写法）
2. 带纹理采样的 FS
3. 带 UBO 的 VS
4. 带分支/循环的 FS
5. 带 SSBO/原子的 CS
6. 带 `OpPhi` 的 FS（`--optimize`，保留 NIR `phi`）
7. 带 `discard` / `demote` 的 FS（`@terminate` + `@demote`）
8. 带 push constant 的 FS（`nir_var_mem_push_const`）
9. 纹理变体 FS：cube / 2D array / shadow，以及 `txl` / `txd` / `txf` / `tg4`

文本允许的唯一差异：无。SSA 编号必须与 `nir_print_shader` 一致，因此遍历顺序固定。JSON 必须稳定编号。

每次改遍历器、打印机或 schema，跑黄金件 diff。

### 常见缺口（后续黄金件）

黄金件 1–9 覆盖了 FS / VS / CS 主路径：ALU、2D `tex`、UBO、if/loop、SSBO atomic、`--optimize` 下的 `phi`、`discard`/`demote`、push constant、cube/array/shadow 与 `txl`/`txd`/`txf`/`tg4`。下面这些在真实 Vulkan/GLSL 里很常见，当前没有黄金件，walker/打印机只按通用路径走，未对照过 dump。

每个缺口单独一份 shader；验收与现有黄金件相同：`--emit ours` 与 Mesa dump `diff` 空，JSON 读回再打印仍空。改 walker / 打印机 / schema 后，新旧黄金件一起回归。

- [x] 7. `discard` / `demote`（alpha 测试 FS）
  - [x] 7.1 `shaders/discard.frag`：`GL_EXT_demote_to_helper_invocation`；`a < 0` 走 `discard`，`a < 0.5` 走 `demote`
  - [x] 7.2 L1 默认 `discard_is_demote=false`：GLSL `discard` → `@terminate`，`demote` → `@demote`；头里无 `uses_discard`（未跑 `nir_shader_gather_info`）
  - [x] 7.3 两条都在 if 内，else 为空块；后续仍 `@store_deref` 颜色
  - [x] 7.4 Wine：mesa vs ours vs `--from-json`，`diff` 为空；黄金件 1–6 回归
- [x] 8. push constant（VS 或 FS）
  - [x] 8.1 `shaders/push.frag`：`layout(push_constant) uniform Push { vec4 tint; }`，颜色乘 tint
  - [x] 8.2 `decl_var push_const`：无 location 括号；`deref_var &p (push_const Push)` + `deref_struct &%2->tint` + `@load_deref`
  - [x] 8.3 shader 头打 `uniforms: 16`（`nir->num_uniforms`，push constant 字节数）
  - [x] 8.4 Wine：mesa vs ours vs `--from-json`，`diff` 为空；黄金件 1–7 回归
- [x] 9. 纹理变体（在黄金件 2 的 2D `tex` 之外）
  - [x] 9.1 `shaders/texvar.frag`：`samplerCube` → `, Cube`；`sampler2DArray` → `, 2D`（array 在类型名）；`sampler2DShadow` 降成 `sampler2D` + `(comparator)`
  - [x] 9.2 `shaders/texop.frag`：`textureLod` → `txl` + `(lod)`；`textureGrad` → `txd` + `(ddx)`/`(ddy)`；`textureOffset` 仍是 `tex` + `(offset)`；`texelFetch` → `txf` + `(lod)`；`textureGather` → `tg4` + `(gather_component)`
  - [x] 9.3 Wine：mesa vs ours vs `--from-json`，`diff` 为空；黄金件 1–8 回归
- [x] 10. image load/store（CS）：`image2D` 的 `@image_deref_load` / `@image_deref_store`（与 SSBO atomic 分开）
  - [x] 10.1 `shaders/image.comp`：`rgba32f` `imageLoad` / `imageStore`
  - [x] 10.2 `decl_var image` 打 `r32g32b32a32_float`（`util_format_short_name`）；JSON `image_format` 可选字段
  - [x] 10.3 intrinsic：`image_dim=2D`、`image_array=false`、`format=r32g32b32a32_float`、`dest_type`/`src_type=float32`；coord 用 `undefined` 填 `vec4`
  - [x] 10.4 Wine：mesa vs ours vs `--from-json`，`diff` 为空；黄金件 1–9 回归
- [x] 11. shared + barrier（CS）：`shared` 数组、`@barrier`、`memory_scope` / `memory_semantics`
  - [x] 11.1 `shaders/shared.comp`：`shared uint tile[64]`，写后 `barrier()` 再读回 SSBO
  - [x] 11.2 `decl_var shared` 无 location 括号；`deref_var &tile (shared uint[64])`
  - [x] 11.3 `@barrier (execution_scope=WORKGROUP, memory_scope=WORKGROUP, mem_semantics=ACQ|REL, mem_modes=shared)`
  - [x] 11.4 Wine：mesa vs ours vs `--from-json`，`diff` 为空；黄金件 1–10 回归
- [x] 12. 常用 system value
  - [x] 12.1 FS：`shaders/sysval.frag`：`gl_FragCoord` / `gl_FrontFacing` / `gl_FragDepth`；头打 `origin_upper_left: true`、`depth_layout: 1`（写 depth 时 `FRAG_DEPTH_LAYOUT_ANY`）
  - [x] 12.2 VS：`shaders/sysval.vert`：`gl_VertexIndex` / `gl_InstanceIndex`（`SYSTEM_VALUE_VERTEX_ID` / `SYSTEM_VALUE_INSTANCE_INDEX`）
  - [x] 12.3 Wine：mesa vs ours vs `--from-json`，`diff` 为空；黄金件 1–11 回归
- [ ] 13. 插值修饰：`flat` / `noperspective`（`INTERP_MODE_*` 不是 `NONE`）
- [ ] 14. 几何阶段（整 stage 尚未打到）
  - [ ] 14.1 GS：`EmitVertex` / `EndPrimitive`，`gl_Position` 写出
  - [ ] 14.2 TCS / TES：`gl_TessLevel*`、`gl_TessCoord`、patch 变量
- [ ] 15. 默认 L1 的 `OpPhi`：不加 `--optimize`，SPIR-V 前端把 phi 降成 `function_temp` store/load（与黄金件 6 对照）
- [ ] 16. `undef` 与未定义 SSA：显式 `nir_undef` 或未写满的 phi 源
- [ ] 17. `switch` / `continue`：多路分支、循环 `continue`（黄金件 4 只有 if + for-break）
- [ ] 18. input attachment（subpass FS）：`subpassLoad`、`@load_output` / input attachment deref
- [ ] 19. 整数 / 转换：`int`/`uint` 算术、`floatBitsToUint` / `bitcast`、8/16/64-bit
- [ ] 20. UBO 动态索引：`ubo[i].member` 或 `ubo.arr[nonconst]`（黄金件 3 是常量偏移 mat4）

## 5. 建议工期（一人，熟悉 Mesa）

| 阶段 | 大约 | 风险 |
|------|------|------|
| 0 交叉编出 exe | 2–4 天 | MinGW cross file / Wine 冒烟 |
| 1 generic dump | 1–2 天 | pass 列表取舍 |
| 2 遍历 + 自有 IR + 打印机 | 3–5 天 | intrinsic/tex/deref 种类多 |
| 3 JSON（对照） | 2–3 天 | schema 稳定 |
| 4 GLSL + 打包 | 1–2 天 | 运行时 DLL |

阶段 2 是风险点。先把黄金件 1 打通（纯 ALU，`diff` 为空），再按指令类型补，不要一上来求全覆盖。

## 6. 明确不做

- 不解析 `nir_serialize()` 二进制
- 不实现自有二进制存储格式（FlatBuffers / TLV / `--emit bin`）；交付物停在进程内自有 IR，JSON 仅为对照
- 不把 NIR 降到具体 GPU 后端
- 不在本计划内实现硬件指令选择 / 寄存器分配
- 不把 OpenGL compatibility GLSL 直接喂给 `glsl_to_nir`（GLSL 统一经 glslang 走 Vulkan SPIR-V）

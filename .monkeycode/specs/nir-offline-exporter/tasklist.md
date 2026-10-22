# Mesa NIR 离线编译器：自有 IR 导出计划书

## 1. 目标

在 Windows 上交付一个离线工具：接受 SPIR-V（GLSL 经 glslang 转为 SPIR-V 后进入同一路径），调用 Mesa 的 `spirv_to_nir()`，在硬件无关层（generic NIR / L1）立刻遍历内存中的 `nir_shader`，映射到自有 IR，再导出 JSON / 自有二进制。

**自有 IR 的主验收目标：** 对自有 IR 再打印一遍，文本与 `nir_print_shader()` 的 NIR dump 一致。打印内容对齐即视为映射做对。

```text
.vert/.frag/.comp  --glslangValidator-->  .spv
.spv               --spirv_to_nir()-->  nir_shader (L1, 硬件无关)
                         |
                         +--> nir_print_shader()              --> a.nir.txt
                         |
                         +--> 遍历 --> 自有 IR --> 自有打印机 --> b.nir.txt
                         |                 |
                         |                 +--> JSON
                         |                 +--> 自有二进制
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
- 编译环境：Linux 上 MinGW 交叉编译，产出 Windows PE（`spirv2nir.exe`）。本机不搭 VS。阶段 0–4 用 Wine 冒烟；干净 Win10/11 实机验收放到阶段 5。

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

### 阶段 4：自有二进制（存储格式）

**做：**

- 选 FlatBuffers 或手写 TLV，字段与 JSON 同构
- 自带 `version` + `mesa_git`
- 提供 `encode` / `decode`，decode 回到阶段 2 同一套结构
- CLI：`spirv2nir shader.spv --emit bin -o shader.nirbin`
- 解码器可独立于 Mesa 运行时（静态链或独立小库）

**产物：** `shader.nirbin` + 编解码库

**验收：**

- `encode(ir) → decode → encode` 字节级一致
- `decode → 自有打印机` 的文本仍与原始 NIR dump 一致
- 故意改 `mesa_git` / `version` 必须拒绝加载
- 不依赖 Mesa 运行时即可 decode

### 阶段 5：GLSL 入口与 Windows 分发

**做：**

- 输入 `.vert/.frag/.comp` 时调用 `glslangValidator -V`，再走同一条 NIR 路径
- `.spv` 直接走
- 静态链 MinGW 运行时（`-static` 或带上 `libgcc_s_seh-1.dll` / `libwinpthread-1.dll` / `libstdc++-6.dll`）
- 打包 zip：`spirv2nir.exe` + 必要 MinGW DLL + `glslangValidator.exe`（或写明需 Vulkan SDK）
- CLI 支持 `--emit text|json|bin`
- 阶段 0–4 继续用 Wine 回归；本阶段用一台干净 Win10/11 或 CI `windows-latest` 做实机验收

**产物：** Windows 分发包

**验收：**

- 同一份 GLSL：`glslang → spirv2nir` 的 NIR dump 与自有 IR 打印文本一致
- Wine 与干净 Win10/11（无 Mesa 源码、无 VS）都能跑黄金件
- `--emit text|json|bin` 三种输出都可用
- `ldd`/`objdump -p` 确认未链 Linux 系统库

## 4. 黄金件与回归

最少 5 个输入，全部同时出 `nir.txt`（Mesa 打印）和 `ours.txt`（自有 IR 打印），CI 对这两份做 diff。JSON / 二进制由自有 IR 再导出，decode 后仍须打出与 `nir.txt` 相同的文本。

1. 纯 ALU fragment（`color * 0.5` 的 Vulkan 写法）
2. 带纹理采样的 FS
3. 带 UBO 的 VS
4. 带分支/循环的 FS
5. 带 SSBO/原子的 CS

文本允许的唯一差异：无。SSA 编号必须与 `nir_print_shader` 一致，因此遍历顺序固定。JSON/二进制必须稳定编号。

每次改遍历器、打印机或 schema，跑黄金件 diff。

## 5. 建议工期（一人，熟悉 Mesa）

| 阶段 | 大约 | 风险 |
|------|------|------|
| 0 交叉编出 exe | 2–4 天 | MinGW cross file / Wine 冒烟 |
| 1 generic dump | 1–2 天 | pass 列表取舍 |
| 2 遍历 + 自有 IR + 打印机 | 3–5 天 | intrinsic/tex/deref 种类多 |
| 3 JSON | 2–3 天 | schema 稳定 |
| 4 自有二进制 | 3–5 天 | 版本兼容 |
| 5 GLSL + 打包 | 1–2 天 | 运行时 DLL |

阶段 2 是风险点。先把黄金件 1 打通（纯 ALU，`diff` 为空），再按指令类型补，不要一上来求全覆盖。

## 6. 明确不做

- 不解析 `nir_serialize()` 二进制
- 不把 NIR 降到具体 GPU 后端
- 不在本计划内实现硬件指令选择 / 寄存器分配
- 不把 OpenGL compatibility GLSL 直接喂给 `glsl_to_nir`（GLSL 统一经 glslang 走 Vulkan SPIR-V）

# mGBA WebAssembly 重新编译指南 + Claude 指令模板

本文件用于：
1. 你自己手动重新编译 `wasm/mgba.js` + `wasm/mgba.wasm`。
2. 把内容复制给 Claude，作为继续工作或排查编译问题的上下文提示词。

---

## 一、环境要求

- **Emscripten SDK**：默认路径 `D:\Codes\emsdk`（用 `emsdk install latest && emsdk activate latest` 安装）
- **Ninja / CMake**：用 `pip install cmake ninja` 安装即可，脚本会自动定位
- **PowerShell 7+**（用于执行 `build-wasm.ps1`）

> 详细的从零搭建步骤、微软商店 python 桩踩坑处理、常见环境问题速查见 [WASM_BUILD_SUMMARY.md](WASM_BUILD_SUMMARY.md) 的"四、环境搭建"。

如果你的 emsdk 路径不同，用 `.\build-wasm.ps1 -EmSdkDir <路径>` 指定。

---

## 二、一键编译

```powershell
.\build-wasm.ps1
```

脚本会自动：
1. 激活 emsdk（绕开 Windows 微软商店 python 桩）。
2. 用 CMake + Ninja 交叉编译 `build-wasm/libmgba.a`（GBA-only、无 pthread、无 debugger、无 scripting）。
3. 用 `emcc` 把 `wasm/mgba-wasm.c` 和 `libmgba.a` 链接成 `wasm/mgba.js` + `wasm/mgba.wasm`。

编译完成后运行验证（需自备 ROM 路径）：

```bash
node wasm/test-node.js
```

---

## 三、如果只想改前端，不需要重新编译

`wasm/mgba.js` 和 `wasm/mgba.wasm` 是核心引擎。如果你只修改 `wasm/index.html`、`wasm/mgba-api.ts`、`wasm/test-node.js`，不需要重新编译 C 代码。

---

## 四、关键编译选项（不要改错）

| 层级 | 关键选项 |
|------|----------|
| CMake | `-DUSE_PTHREADS=OFF` |
| CMake | `-DENABLE_DEBUGGERS=OFF -DENABLE_SCRIPTING=OFF` |
| CMake | `-DM_CORE_GBA=ON -DM_CORE_GB=OFF` |
| CFLAGS | `-DPATH_MAX=4096 -D_GNU_SOURCE -DDISABLE_THREADING -DDISABLE_ANON_MMAP` |
| emcc | `-s EMULATE_FUNCTION_POINTER_CASTS=1 -s ALLOW_MEMORY_GROWTH=1` |
| emcc | `-s MODULARIZE=1 -s EXPORT_NAME="createModule"` |
| emcc | `EXPORTED_FUNCTIONS` 和 `EXPORTED_RUNTIME_METHODS` 必须包含当前所有导出函数 |

桥接器 `wasm/mgba-wasm.c` 编译时的宏定义必须与 `libmgba.a` 完全一致：

```bash
-DBUILD_STATIC -DENABLE_DIRECTORIES -DENABLE_VFS -DENABLE_VFS_FD \
-DHAVE_FREELOCALE -DHAVE_FUTIMENS -DHAVE_LOCALE -DHAVE_LOCALTIME_R \
-DHAVE_NEWLOCALE -DHAVE_REALPATH -DHAVE_SETLOCALE -DHAVE_STRDUP \
-DHAVE_STRLCPY -DHAVE_STRNDUP -DHAVE_STRTOF_L -DHAVE_USELOCALE \
-DHAVE_VASPRINTF -DHAVE_XLOCALE -DM_CORE_GBA
```

---

## 五、常见错误

| 错误 | 原因 | 解决 |
|------|------|------|
| `memory access out of bounds` / `table index is out of bounds` | 桥接器与 libmgba.a 宏定义不一致，或 pthread/mmap 问题 | 检查 `-DENABLE_DIRECTORIES`、`-DENABLE_VFS`、`-DDISABLE_ANON_MMAP`、`-DUSE_PTHREADS=OFF` |
| `null function or function signature mismatch` | `mCore` 虚表布局不一致 | 确保桥接器 emcc 命令的宏与 CMake 完全一致 |
| `ninja: command not found` | Ninja 未安装 | 执行 `pip install ninja` |
| `PATH_MAX` / `strdup` 未定义 | 缺少 GNU 宏 | 加 `-D_GNU_SOURCE -DPATH_MAX=4096` |
| 编译产物太大 | 调试构建 | 发布时把 `-O0 -g3` 换成 `-O3` |

---

## 六、给 Claude 的指令模板

下次需要我（Claude）继续处理时，把下面整段复制到对话框：

```
本项目是 mGBA 的 GBA-only WebAssembly 构建，origin 为 https://github.com/zb2947244682/mgba。

请基于 WASM_BUILD_SUMMARY.md 和 wasm/README.md 继续工作。

当前目标：
[ ] 重新编译 wasm 产物
[ ] 修复 wasm 运行时崩溃
[ ] 添加 XX 功能（例如：音频输出、虚拟按键、存档到 localStorage）
[ ] 优化构建体积/性能
[ ] 其他：__________

环境信息：
- emsdk 路径：D:\Codes\emsdk
- 一键编译脚本：build-wasm.ps1（PowerShell）
- 验证脚本：node wasm/test-node.js
- 测试 ROM：D:\Media\Downloads\究极绿宝石.gba

注意事项：
- 仅支持 GBA 核心
- 必须禁用 pthread
- 必须加 -DDISABLE_ANON_MMAP
- 必须加 -s EMULATE_FUNCTION_POINTER_CASTS=1
- 桥接器与 libmgba.a 的宏定义必须一致
```

---

## 七、快速检查清单

重新编译前确认：

- [ ] emsdk 已安装且路径正确（默认 `D:\Codes\emsdk`）
- [ ] `pip install cmake ninja` 已执行（脚本自动定位，无需手改路径）
- [ ] 执行 `.\build-wasm.ps1` 无报错
- [ ] `wasm/mgba.js` 和 `wasm/mgba.wasm` 时间戳已更新

---

## 八、发布构建建议

调试构建体积大、速度慢。发布时把 `build-wasm.ps1` 中的（用 `-DebugBuild` 开关）：

```bash
-s ASSERTIONS=2 \
-O0 \
-g3 \
```

改成：

```bash
-O3 \
```

可以显著减小 `.wasm` 体积并提升运行帧率。

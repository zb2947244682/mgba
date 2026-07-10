# mGBA 纯 WebAssembly GBA 核心构建总结

> 目标：将 mGBA 的 C 代码库编译为可在浏览器/微信小程序环境中使用的纯 `.wasm` + `.js` 库，仅保留 GBA 核心，提供 TypeScript/JavaScript API。

---

## 一、已完成内容

1. **成功编译出 GBA-only 的 WebAssembly 库**
   - 生成 `wasm/mgba.js`（JS 加载器）和 `wasm/mgba.wasm`（Wasm 字节码）。
   - 静态库 `build-wasm/libmgba.a` 约 1 MB（已关闭调试器、脚本、GL 等模块）。

2. **C 桥接层 `wasm/mgba-wasm.c`**
   暴露给 JS 的 API 包括：
   - `mgba_create()` / `mgba_destroy(core)`
   - `mgba_load_rom(core, ptr, size)` —— 加载 ROM，内部复制数据
   - `mgba_reset(core)` / `mgba_run_frame(core)`
   - `mgba_set_video_buffer(core, ptr, stride)`
   - `mgba_get_video_width()` / `mgba_get_video_height()`
   - `mgba_set_keys()` / `mgba_clear_keys()`
   - `mgba_state_size()` / `mgba_save_state()` / `mgba_load_state()`
   - `mgba_get_audio_sample_rate()` / `mgba_read_audio()`
   - 按键常量导出：`mgba_key_a` / `mgba_key_b` / ...

3. **TypeScript 封装 `wasm/mgba-api.ts`**
   - 提供 `createMgba()` 返回 `MgbaInstance`。
   - 封装 ROM 加载、帧运行、视频缓冲拷贝、存档、按键。

4. **浏览器 Demo `wasm/index.html`**
   - 支持文件选择加载 ROM、Canvas 渲染、键盘控制、存档/读档按钮。

5. **Node.js 验证脚本 `wasm/test-node.js`**
   - 在不依赖浏览器的情况下验证核心创建、ROM 加载、帧运行、渲染输出、安全销毁。

6. **一键编译脚本 `build-wasm.ps1`**（Windows / PowerShell，替代旧版 `build-wasm.sh`）
   - 从配置 CMake、编译静态库到链接 Wasm 桥接层，全流程自动化。

---

## 二、关键修改

### 1. `CMakeLists.txt`：允许命令行关闭 pthread

原代码在 UNIX 分支强制开启 pthread，即使传入 `-DUSE_PTHREADS=OFF` 也不生效：

```cmake
elseif(UNIX)
    set(USE_PTHREADS ON)
```

改为：

```cmake
elseif(UNIX)
    if(NOT DEFINED USE_PTHREADS)
        set(USE_PTHREADS ON)
    endif()
```

这样 Emscripten 构建可以真正禁用 pthread，避免线程相关符号和函数指针 ABI 问题。

### 2. Emscripten 构建选项

| 层级 | 关键选项 | 原因 |
|------|----------|------|
| CMake | `-DUSE_PTHREADS=OFF` | 禁用 pthread |
| CMake | `-DENABLE_DEBUGGERS=OFF -DENABLE_SCRIPTING=OFF` | 最小化核心 |
| CMake | `-DM_CORE_GBA=ON -DM_CORE_GB=OFF` | 仅保留 GBA |
| CFLAGS | `-DPATH_MAX=4096 -D_GNU_SOURCE` | 解决 Emscripten 下 PATH_MAX/strdup 缺失 |
| CFLAGS | `-DDISABLE_THREADING` | 关闭 mGBA 内部线程代码 |
| CFLAGS | `-DDISABLE_ANON_MMAP` | 让 `anonymousMemoryMap` 退化为 `calloc/free`，兼容 Emscripten |
| emcc | `-s EMULATE_FUNCTION_POINTER_CASTS=1` | 避免函数指针签名不匹配 |
| emcc | `-s ALLOW_MEMORY_GROWTH=1` | 32MB ROM 需要动态扩容 |
| emcc | `-s EXPORTED_RUNTIME_METHODS` 包含 `HEAPU8` 等 | 方便 JS 读写 Wasm 内存 |

### 3. 桥接器宏定义与 `libmgba.a` 严格一致

`mgba-wasm.c` 编译时显式定义了与静态库相同的宏，确保 `mCore`、`VFile` 等结构体布局一致：

```bash
-DBUILD_STATIC -DENABLE_DIRECTORIES -DENABLE_VFS -DENABLE_VFS_FD \
-DHAVE_FREELOCALE -DHAVE_FUTIMENS ... -DM_CORE_GBA
```

### 4. ROM 加载方式

`mgba_load_rom` 使用 `VFileMemChunk` 在 C 层复制 ROM 数据，由核心在 `deinit` 时统一释放。这样 JS/TS 层传入 ROM 后即可释放原始指针，不需要长期保留 Uint8Array。

---

## 三、验证结果

使用 ROM：`D:\Media\Downloads\究极绿宝石.gba`（32 MB）

```text
Creating core...
Core created at 2188968
Video size: 240 x 160
Video buffer set
ROM size: 33554432
ROM loaded: 1
Core reset
Running 60 frames...
First pixel after 60 frames: 0 82 148
Test passed
```

- 核心初始化成功。
- ROM 加载成功。
- 60 帧运行正常，有实际画面输出（像素值非全白/全黑）。
- `destroy` 后无内存崩溃。

单元测试：

```text
fullInit: 2
gbaCreate: 1
armInit: 1
```

---

## 四、环境搭建

> 本节面向"拿到一台干净的 Windows 机器，从零搭起编译环境"。已验证组合见 `wasm/README.md` 的"已验证环境"小节，下面是可复现的搭建步骤。

### 1. 操作系统与 Shell

- Windows 10/11，使用 **PowerShell** 执行 `build-wasm.ps1`。
- Git Bash 可用于跑 `node`、`python -m http.server` 等命令，但编译脚本本身用 PowerShell。
- 仓库要求 PowerShell 7+（pwsh），Windows 11 自带，或从 GitHub Releases 安装。

### 2. 安装 Emscripten SDK（emsdk）

emsdk 是 Emscripten（emcc）的官方安装器，推荐装到固定目录（脚本默认 `D:\Codes\emsdk`）：

```powershell
git clone https://github.com/emscripten-core/emsdk.git D:\Codes\emsdk
cd D:\Codes\emsdk
.\emsdk install latest
.\emsdk activate latest
```

激活后会在 `D:\Codes\emsdk\upstream\emscripten\` 下生成 `emcc.exe`，并自带一个 `python` 与 `node`。

> 踩坑：Windows 自带的"微软商店 python 桩"会被 `emcc.bat` 误调用，导致激活时报 python 相关错误或弹出商店窗口。`build-wasm.ps1` 已通过显式把 emsdk 自带 python 目录塞到 `PATH` 最前来绕过这个问题，正常情况下无需手动处理。

### 3. 安装 CMake 与 Ninja

`build-wasm.ps1` 用 Ninja 作为 CMake 生成器（比 Make 快很多）。两者都可一键装：

```powershell
pip install cmake ninja
```

脚本会按以下优先级自动定位：
1. `%LOCALAPPDATA%\Python\pythoncore-<版本>-64\Scripts\` 下的 `cmake.exe` / `ninja.exe`
2. `PATH` 中的 `cmake` / `ninja`

如果已装 VS 自带 CMake 或从官网下的独立包，确保它在 `PATH` 即可，无需 pip 重复安装。

> 已验证版本：CMake 4.3.x、Ninja 1.13、emcc 6.0.2。版本不必严格一致，但 emcc 6.x 与 3.x 的导出行为有差异（`_malloc`/`_free` 在 6.x 必须放进 `EXPORTED_FUNCTIONS` 而非 `EXPORTED_RUNTIME_METHODS`，脚本已适配 6.x）。

### 4. 验证环境

```powershell
cd D:\Codes\mgba
.\build-wasm.ps1
```

成功时会输出工具版本、找到 `libmgba.a`、链接出 `wasm/mgba.js` + `wasm/mgba.wasm`，最后提示自检命令。

可选的运行自检（需自备 ROM，并在 `wasm/test-node.js` 里改 ROM 路径）：

```powershell
node wasm\test-node.js
```

### 5. 常见环境问题速查

| 现象 | 原因 | 处理 |
|------|------|------|
| `emsdk activate` 报 python 错误 / 弹微软商店 | 撞上商店 python 桩 | 用 `build-wasm.ps1`（已绕过）；或手动激活后把 `D:\Codes\emsdk\python\<版本>` 加到 PATH 最前 |
| `ninja: command not found` | 没装 Ninja | `pip install ninja` |
| `cmake` 命令找不到 | 没装 CMake | `pip install cmake` |
| `PATH_MAX` / `strdup` 未定义 | 缺少 GNU 宏 | 脚本已含 `-D_GNU_SOURCE -DPATH_MAX=4096`，确认未手改 CFLAGS |
| `memory access out of bounds` / `table index out of bounds` | 桥接器与 `libmgba.a` 宏不一致，或 pthread 残留 | 确认 `-DUSE_PTHREADS=OFF`、`-DDISABLE_ANON_MMAP`，让脚本从 `flags.make` 自动读宏定义 |

---

## 五、如何使用

### 方式 1：浏览器

```bash
# 进入 wasm 目录并启动静态服务器
cd wasm
python -m http.server 8080
# 浏览器访问 http://localhost:8080/index.html
```

加载 ROM 后点击"开始运行"。

### 方式 2：Node.js 验证

```bash
node wasm/test-node.js
```

### 方式 3：TypeScript 封装

```ts
import { createMgba, GBA_KEY } from './wasm/mgba-api';

const mgba = await createMgba();
mgba.loadRom(new Uint8Array(romBuffer));
mgba.reset();

function loop() {
  mgba.runFrame();
  const image = new ImageData(mgba.width, mgba.height);
  mgba.setVideoBuffer(image.data);  // 从 Wasm 内存拷贝到 Canvas
  ctx.putImageData(image, 0, 0);
  requestAnimationFrame(loop);
}
loop();
```

---

## 六、重新编译

```powershell
.\build-wasm.ps1
# 或指定 emsdk 路径：.\build-wasm.ps1 -EmSdkDir D:\Codes\emsdk
# 调试构建：.\build-wasm.ps1 -DebugBuild
```

脚本会：
1. 激活 Emscripten SDK（自动绕开 Windows 微软商店 python 桩）。
2. 用 Ninja + CMake 交叉编译 `libmgba.a`。
3. 用 `emcc` 把 `wasm/mgba-wasm.c` 和静态库链接成 `wasm/mgba.js` + `wasm/mgba.wasm`。

> 注意：脚本默认 emsdk 路径为 `D:/Codes/emsdk`，可用 `-EmSdkDir` 覆盖。cmake/ninja 用 `pip install cmake ninja` 安装即可，脚本会自动定位。默认为 `-O3` 发布构建；加 `-DebugBuild` 切换为 `-O0 -g3` + `ASSERTIONS=2` 调试构建。

---

## 七、文件清单

| 文件 | 作用 |
|------|------|
| `CMakeLists.txt` | 修改 pthread 覆盖逻辑 |
| `build-wasm.ps1` | 一键编译脚本（Windows / PowerShell） |
| `wasm/mgba-wasm.c` | C 桥接层 |
| `wasm/mgba-api.ts` | TypeScript 封装 |
| `wasm/index.html` | 浏览器演示 |
| `wasm/test-node.js` | Node.js 验证 |
| `wasm/README.md` | 使用文档 |
| `wasm/mgba.js` + `wasm/mgba.wasm` | 编译产物 |
| `build-wasm/libmgba.a` | 中间静态库 |

---

## 八、已知限制

- 仅支持 GBA，不支持 GB/GBC。
- 使用 HLE BIOS，未加载官方 BIOS。
- 音频 API 已导出，但 HTML demo 暂未输出音频。
- 发布构建为 `-O3`；调试构建（`-DebugBuild`）会带 `ASSERTIONS=2` 且体积较大，仅用于排查问题。
- 微信小程序中需确保 `WebAssembly` 可用，并将 `.wasm` 文件作为资源加载。

---

## 九、后续建议

1. **音频输出**：接入 `AudioContext`，从 `mgba_read_audio` 读取采样并播放。
2. **存档持久化**：把 `savedState` 写入 `localStorage` 或小程序本地存储。
3. **触屏按键**：在 HTML demo 中添加虚拟手柄，适配移动端/微信小程序。
4. **BIOS 加载**：如需更精确模拟，可暴露 `mgba_load_bios` 加载官方 GBA BIOS。
5. **联机（SIO Link）**：基于 `mCoreSetPeripheral(core, mPERIPH_GBA_LINK_PORT, driver)` 安装自定义 `GBASIODriver`，通过 WebSocket 中继实现互联网联机；参考实现见 `src/gba/sio/lockstep.c`，但需改为异步模型（浏览器单线程不能阻塞主线程）。

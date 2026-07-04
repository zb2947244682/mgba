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

6. **一键编译脚本 `build-wasm.sh`**
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

## 四、如何使用

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

## 五、重新编译

```bash
bash build-wasm.sh
```

脚本会：
1. 激活 Emscripten SDK。
2. 用 Ninja + CMake 交叉编译 `libmgba.a`。
3. 用 `emcc` 把 `wasm/mgba-wasm.c` 和静态库链接成 `wasm/mgba.js` + `wasm/mgba.wasm`。

> 注意：脚本里硬编码了 `D:/Codes/emsdk` 路径。如果你的 emsdk 安装路径不同，请修改 `build-wasm.sh` 顶部的路径变量。

---

## 六、文件清单

| 文件 | 作用 |
|------|------|
| `CMakeLists.txt` | 修改 pthread 覆盖逻辑 |
| `build-wasm.sh` | 一键编译脚本 |
| `wasm/mgba-wasm.c` | C 桥接层 |
| `wasm/mgba-api.ts` | TypeScript 封装 |
| `wasm/index.html` | 浏览器演示 |
| `wasm/test-node.js` | Node.js 验证 |
| `wasm/README.md` | 使用文档 |
| `wasm/mgba.js` + `wasm/mgba.wasm` | 编译产物 |
| `build-wasm/libmgba.a` | 中间静态库 |

---

## 七、已知限制

- 仅支持 GBA，不支持 GB/GBC。
- 使用 HLE BIOS，未加载官方 BIOS。
- 音频 API 已导出，但 HTML demo 暂未输出音频。
- 当前为 `-O0 -g3` 调试构建，体积较大；发布时建议改为 `-O3` 并移除 `-s ASSERTIONS=2`。
- 微信小程序中需确保 `WebAssembly` 可用，并将 `.wasm` 文件作为资源加载。

---

## 八、后续建议

1. **性能优化**：发布构建 `-O3` + `-flto`，可显著减小体积并提升帧率。
2. **音频输出**：接入 `AudioContext`，从 `mgba_read_audio` 读取采样并播放。
3. **存档持久化**：把 `savedState` 写入 `localStorage` 或小程序本地存储。
4. **触屏按键**：在 HTML demo 中添加虚拟手柄，适配移动端/微信小程序。
5. **BIOS 加载**：如需更精确模拟，可暴露 `mgba_load_bios` 加载官方 GBA BIOS。

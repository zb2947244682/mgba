# mGBA WebAssembly 构建

本目录包含把 mGBA 的 GBA 核心编译成纯 WebAssembly 库（`mgba.js` + `mgba.wasm`）所需的桥接代码与示例。

## 文件说明

- `wasm/mgba-wasm.c` — C 桥接层，暴露给 JS/TS 的 API
- `wasm/mgba-api.ts` — TypeScript 封装
- `wasm/index.html` — 浏览器演示页面
- `wasm/test-node.js` — Node.js 测试脚本
- `build-wasm.ps1` — 一键编译脚本（Windows / PowerShell）

## 已验证环境

- Windows 11 + Git Bash
- Emscripten SDK at `D:\Codes\emsdk`
- CMake from emsdk at `D:\Codes\emsdk\cmake\4.2.0-rc3_64bit\bin\cmake.exe`
- Ninja at `D:\Codes\emsdk\ninja\ninja.exe`

## 关键编译选项

| 层级 | 关键设置 | 作用 |
|------|----------|------|
| CMake | `-DUSE_PTHREADS=OFF` | 避免 pthread 导致 Emscripten 下 ABI/指针问题 |
| CMake | `-DENABLE_DEBUGGERS=OFF -DENABLE_SCRIPTING=OFF` | 最小化核心，去掉调试器/脚本 |
| CMake | `-DM_CORE_GBA=ON -DM_CORE_GB=OFF` | 仅保留 GBA |
| C flags | `-DDISABLE_THREADING` | 关闭 mGBA 内部线程代码 |
| C flags | `-DDISABLE_ANON_MMAP` | 让 `anonymousMemoryMap` 使用 `calloc/free`，兼容 Emscripten |
| emcc | `-s EMULATE_FUNCTION_POINTER_CASTS=1` | 避免函数指针签名不匹配 |
| emcc | `-s ALLOW_MEMORY_GROWTH=1` | ROM 最大 32MB，需要动态扩容 |

## JS API

加载后通过全局 `createModule()` 获取模块：

```js
const module = await createModule();
const create = module.cwrap('mgba_create', 'number', []);
const core = create();
```

核心函数：

- `mgba_create()` — 创建并初始化 GBA 核心
- `mgba_destroy(core)` — 销毁核心
- `mgba_load_rom(core, ptr, size)` — 加载 ROM（会内部复制，调用后可释放 ptr）
- `mgba_reset(core)` — 复位
- `mgba_run_frame(core)` — 运行一帧
- `mgba_set_video_buffer(core, ptr, stride)` — 设置视频缓冲
- `mgba_set_keys(core, keys)` / `mgba_clear_keys(core, keys)` — 按键
- `mgba_state_size(core)` / `mgba_save_state(core, ptr)` / `mgba_load_state(core, ptr)` — 存档

## 使用 TypeScript 封装

```ts
import { createMgba, GBA_KEY } from './mgba-api';

const mgba = await createMgba();
mgba.loadRom(new Uint8Array(romBuffer));
mgba.reset();

function frame() {
  mgba.runFrame();
  const image = new ImageData(mgba.width, mgba.height);
  mgba.setVideoBuffer(image.data);
  ctx.putImageData(image, 0, 0);
  requestAnimationFrame(frame);
}
frame();
```

## 运行 Node 测试

```bash
node wasm/test-node.js
```

该脚本会创建核心、加载 `D:\Media\Downloads\究极绿宝石.gba`、运行 60 帧并验证视频输出，最后安全销毁。

## 已知限制

- 仅支持 GBA（`M_CORE_GBA=ON`）
- 使用 HLE BIOS，未加载官方 BIOS
- 音频 API 已导出，但 HTML demo 未播放音频
- 当前为 `-O0 -g3` 调试构建，发布时可改为 `-O3` 并去掉 `-s ASSERTIONS=2`

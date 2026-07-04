export interface MgbaModule {
  cwrap: typeof import('./mgba').cwrap;
  ccall: typeof import('./mgba').ccall;
  getValue: typeof import('./mgba').getValue;
  setValue: typeof import('./mgba').setValue;
  UTF8ToString: typeof import('./mgba').UTF8ToString;
  writeArrayToMemory: typeof import('./mgba').writeArrayToMemory;
  _malloc(size: number): number;
  _free(ptr: number): void;
  HEAPU8: Uint8Array;
  HEAPU16: Uint16Array;
  HEAPU32: Uint32Array;
  HEAP16: Int16Array;
  HEAP32: Int32Array;
}

export interface MgbaInstance {
  core: number;
  module: MgbaModule;
  width: number;
  height: number;
  sampleRate: number;

  loadRom(data: Uint8Array): boolean;
  reset(): void;
  runFrame(): void;
  setVideoBuffer(buffer: Uint8ClampedArray): void;
  setKeys(keys: number): void;
  clearKeys(keys: number): void;
  getState(): Uint8Array | null;
  setState(state: Uint8Array): boolean;
}

export const GBA_KEY = {
  A: 0,
  B: 1,
  SELECT: 2,
  START: 3,
  RIGHT: 4,
  LEFT: 5,
  UP: 6,
  DOWN: 7,
  R: 8,
  L: 9,
} as const;

let modulePromise: Promise<MgbaModule> | null = null;

export async function createMgbaModule(): Promise<MgbaModule> {
  if (modulePromise) return modulePromise;
  // eslint-disable-next-line @typescript-eslint/no-var-requires
  const createModule = require('./mgba.js');
  modulePromise = createModule() as Promise<MgbaModule>;
  return modulePromise;
}

export async function createMgba(): Promise<MgbaInstance> {
  const module = await createMgbaModule();

  const create = module.cwrap('mgba_create', 'number', []);
  const destroy = module.cwrap('mgba_destroy', null, ['number']);
  const loadRom = module.cwrap('mgba_load_rom', 'number', ['number', 'number', 'number']);
  const reset = module.cwrap('mgba_reset', null, ['number']);
  const runFrame = module.cwrap('mgba_run_frame', null, ['number']);
  const getWidth = module.cwrap('mgba_get_video_width', 'number', ['number']);
  const getHeight = module.cwrap('mgba_get_video_height', 'number', ['number']);
  const setVideoBuffer = module.cwrap('mgba_set_video_buffer', null, ['number', 'number', 'number']);
  const getAudioSampleRate = module.cwrap('mgba_get_audio_sample_rate', 'number', ['number']);
  const readAudio = module.cwrap('mgba_read_audio', 'number', ['number', 'number', 'number']);
  const setKeys = module.cwrap('mgba_set_keys', null, ['number', 'number']);
  const clearKeys = module.cwrap('mgba_clear_keys', null, ['number', 'number']);
  const stateSize = module.cwrap('mgba_state_size', 'number', ['number']);
  const saveState = module.cwrap('mgba_save_state', 'number', ['number', 'number']);
  const loadState = module.cwrap('mgba_load_state', 'number', ['number', 'number']);

  const core = create();
  if (!core) {
    throw new Error('Failed to create mGBA core');
  }

  const width = getWidth(core);
  const height = getHeight(core);
  const sampleRate = getAudioSampleRate(core);

  // Pre-allocate a video buffer in WASM memory
  const videoBufferPtr = module._malloc(width * height * 4);
  setVideoBuffer(core, videoBufferPtr, width);

  const stateBufPtr = stateSize(core);

  const instance: MgbaInstance = {
    core,
    module,
    width,
    height,
    sampleRate,

    loadRom(data: Uint8Array): boolean {
      const ptr = module._malloc(data.byteLength);
      module.HEAPU8.set(data, ptr);
      const ok = loadRom(core, ptr, data.byteLength);
      module._free(ptr);
      return ok !== 0;
    },

    reset(): void {
      reset(core);
    },

    runFrame(): void {
      runFrame(core);
    },

    setVideoBuffer(buffer: Uint8ClampedArray): void {
      if (buffer.length !== width * height * 4) {
        throw new Error(`Video buffer size mismatch: expected ${width * height * 4}, got ${buffer.length}`);
      }
      buffer.set(module.HEAPU8.subarray(videoBufferPtr, videoBufferPtr + buffer.length));
    },

    setKeys(keys: number): void {
      setKeys(core, keys);
    },

    clearKeys(keys: number): void {
      clearKeys(core, keys);
    },

    getState(): Uint8Array | null {
      const size = stateSize(core);
      if (!size) return null;
      const ptr = module._malloc(size);
      const ok = saveState(core, ptr);
      if (!ok) {
        module._free(ptr);
        return null;
      }
      const state = new Uint8Array(module.HEAPU8.subarray(ptr, ptr + size));
      module._free(ptr);
      return state;
    },

    setState(state: Uint8Array): boolean {
      const size = stateSize(core);
      if (state.length !== size) return false;
      const ptr = module._malloc(size);
      module.HEAPU8.set(state, ptr);
      const ok = loadState(core, ptr);
      module._free(ptr);
      return ok !== 0;
    },
  };

  // Ensure cleanup on page unload
  const cleanup = () => destroy(core);
  if (typeof window !== 'undefined') {
    window.addEventListener('beforeunload', cleanup);
  }

  return instance;
}

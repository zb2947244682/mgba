const fs = require('fs');
const path = require('path');
const createModule = require('./mgba.js');

const ROM_PATH = process.env.ROM_PATH || 'D:/Media/Downloads/究极绿宝石.gba';

createModule().then(module => {
  const cwrap = module.cwrap;
  const create = cwrap('mgba_create', 'number', []);
  const destroy = cwrap('mgba_destroy', null, ['number']);
  const loadRom = cwrap('mgba_load_rom', 'number', ['number', 'number', 'number']);
  const reset = cwrap('mgba_reset', null, ['number']);
  const runFrame = cwrap('mgba_run_frame', null, ['number']);
  const getVideoWidth = cwrap('mgba_get_video_width', 'number', ['number']);
  const getVideoHeight = cwrap('mgba_get_video_height', 'number', ['number']);
  const setVideoBuffer = cwrap('mgba_set_video_buffer', null, ['number', 'number', 'number']);

  console.log('Creating core...');
  const core = create();
  if (!core) throw new Error('mgba_create returned null');
  console.log('Core created at', core);

  const width = getVideoWidth(core);
  const height = getVideoHeight(core);
  console.log('Video size:', width, 'x', height);

  const stride = width;
  const bufferSize = width * height * 4;
  const buffer = module._malloc(bufferSize);
  setVideoBuffer(core, buffer, stride);
  console.log('Video buffer set');

  const rom = fs.readFileSync(ROM_PATH);
  console.log('ROM size:', rom.length);
  const romPtr = module._malloc(rom.length);
  module.HEAPU8.set(rom, romPtr);
  const loaded = loadRom(core, romPtr, rom.length);
  console.log('ROM loaded:', loaded);
  module._free(romPtr); // VFileMemChunk copied the data

  reset(core);
  console.log('Core reset');

  console.log('Running 60 frames...');
  for (let i = 0; i < 60; i++) {
    runFrame(core);
  }

  const r = module.HEAPU8[buffer];
  const g = module.HEAPU8[buffer + 1];
  const b = module.HEAPU8[buffer + 2];
  console.log('First pixel after 60 frames:', r, g, b);

  destroy(core);
  module._free(buffer);
  console.log('Test passed');
}).catch(err => {
  console.error('Test failed:', err);
  process.exit(1);
});

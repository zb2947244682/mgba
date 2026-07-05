const fs = require('fs');
const createModule = require('./mgba.js');

const ROM_PATH = 'D:/Media/Downloads/GBA中文游戏/牧场物语 - 矿石镇的伙伴们[CGP&模拟天下](简)(JP)(64Mb).gba';

function dumpSave(module, fns, core, label) {
  const size = fns.getSaveSize(core);
  console.log(`[${label}] getSaveSize = ${size}`);
  if (!size || size > 2 * 1024 * 1024) {
    console.log(`[${label}] skip read (size 0 or too big)`);
    return null;
  }
  const ptr = module._malloc(size);
  const actual = fns.readSave(core, ptr, size);
  const data = Buffer.from(module.HEAPU8.subarray(ptr, ptr + actual));
  module._free(ptr);
  let nonff = 0, nonZero = 0;
  for (let i = 0; i < data.length; i++) {
    if (data[i] !== 0xff) nonff++;
    if (data[i] !== 0x00) nonZero++;
  }
  console.log(`[${label}] readSave actual=${actual}, nonFF=${nonff}, nonZero=${nonZero}, head=[${data.slice(0, 16).join(',')}]`);
  return data;
}

createModule().then(module => {
  const cwrap = module.cwrap;
  const fns = {
    createRaw: cwrap('mgba_create_raw', 'number', []),
    initCore: cwrap('mgba_init_core', 'number', ['number']),
    destroy: cwrap('mgba_destroy', null, ['number']),
    loadRom: cwrap('mgba_load_rom', 'number', ['number', 'number', 'number']),
    loadSave: cwrap('mgba_load_save', 'number', ['number', 'number', 'number']),
    writeSave: cwrap('mgba_write_save', 'number', ['number', 'number', 'number']),
    reset: cwrap('mgba_reset', null, ['number']),
    runFrame: cwrap('mgba_run_frame', null, ['number']),
    getSaveSize: cwrap('mgba_get_save_size', 'number', ['number']),
    readSave: cwrap('mgba_read_save', 'number', ['number', 'number', 'number']),
  };

  const core = fns.createRaw();
  if (!core) throw new Error('create_raw null');
  if (!fns.initCore(core)) throw new Error('init_core failed');

  const rom = fs.readFileSync(ROM_PATH);
  console.log('ROM size:', rom.length);
  const romPtr = module._malloc(rom.length);
  module.HEAPU8.set(rom, romPtr);
  const loaded = fns.loadRom(core, romPtr, rom.length);
  console.log('ROM loaded:', !!loaded);
  module._free(romPtr);

  console.log('\n=== 阶段0：loadROM 之后（reset 之前）===');
  dumpSave(module, fns, core, 'after-loadROM');

  console.log('\n=== 阶段1：构造 32KB 带标记的存档，调用 mgba_load_save ===');
  const sram32 = Buffer.alloc(32 * 1024);
  for (let i = 0; i < sram32.length; i++) sram32[i] = (i * 7 + 3) & 0xff;
  sram32[0] = 0xDE; sram32[1] = 0xAD; sram32[2] = 0xBE; sram32[3] = 0xEF;
  sram32[0x100] = 0xCA; sram32[0x101] = 0xFE;
  const sramPtr = module._malloc(sram32.length);
  module.HEAPU8.set(sram32, sramPtr);
  const lok = fns.loadSave(core, sramPtr, sram32.length);
  console.log('loadSave(32KB) returned:', lok);
  module._free(sramPtr);

  const afterLoad = dumpSave(module, fns, core, 'after-loadSave');
  if (afterLoad) {
    const head = afterLoad[0] === 0xDE && afterLoad[1] === 0xAD && afterLoad[2] === 0xBE && afterLoad[3] === 0xEF;
    const mid = afterLoad[0x100] === 0xCA && afterLoad[0x101] === 0xFE;
    console.log('>>> loadSave 后能读回标记头?', head, '| 0x100 标记?', mid, '(true=vf 已设置并可见)');
  }

  console.log('\n=== 阶段2：reset（验证 reset 不清掉 vf）===');
  fns.reset(core);
  const afterReset = dumpSave(module, fns, core, 'after-reset');
  if (afterReset) {
    const head = afterReset[0] === 0xDE && afterReset[1] === 0xAD && afterReset[2] === 0xBE && afterReset[3] === 0xEF;
    console.log('>>> reset 后标记头仍在?', head, '(true=reset 不破坏 vf)');
  }

  console.log('\n=== 阶段3：跑 1200 帧（让游戏初始化 save type）===');
  for (let i = 0; i < 1200; i++) fns.runFrame(core);
  const afterRun = dumpSave(module, fns, core, 'after-1200-frames');
  if (afterRun) {
    const head = afterRun[0] === 0xDE && afterRun[1] === 0xAD && afterRun[2] === 0xBE && afterRun[3] === 0xEF;
    console.log('>>> 跑帧后标记头仍在?', head, '(若游戏已写存档到该区域则可能被覆盖，属正常)');
    console.log('>>> 关键：getSaveSize 非 0 =', afterRun.length > 0, 'size=', afterRun.length, '（类型已检测）');
  }

  fns.destroy(core);
  console.log('\nDone.');
}).catch(err => {
  console.error('Test failed:', err);
  process.exit(1);
});

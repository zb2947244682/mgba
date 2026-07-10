// 权威检测：实例化 mGBA module 后，看 emscripten 如何暴露 SIO 导出。
// 前端 useNetplay 的 hasSio 用 typeof m['_mgba_sio_attach'] === 'function' 检测。
// 本脚本验证该直接属性是否存在，以及 cwrap 路径是否可用。
const createModule = require('./mgba.js');

const SIO = ['mgba_sio_attach', 'mgba_sio_set_peer', 'mgba_sio_detach', 'mgba_sio_on_peer'];

(async () => {
  const m = await createModule({ print: () => {}, printErr: () => {} });

  console.log("=== module._<name> 直接属性检测（前端 hasSio 用的方式）===");
  for (const n of SIO) {
    const v = m['_' + n];
    console.log(`  m._${n}: ${typeof v}`);
  }

  console.log("\n=== cwrap 路径检测 ===");
  for (const n of SIO) {
    try {
      const fn = m.cwrap(n, 'number', ['number']);
      console.log(`  cwrap(${n}): ${typeof fn}`);
    } catch (e) {
      console.log(`  cwrap(${n}) THROW: ${e.message}`);
    }
  }

  console.log("\n=== wasm 导出表（前 60 个名）===");
  const bin = require('fs').readFileSync(require('path').join(__dirname, 'mgba.wasm'));
  const mod = new WebAssembly.Module(bin);
  const ex = WebAssembly.Module.exports(mod);
  console.log("  total:", ex.length);
  console.log("  names:", ex.map(e => e.name).join(', '));

  // 找含 sio/mgba 的导出名（可能是原名，也可能被 minify）
  const hit = ex.map(e => e.name).filter(n => n.includes('sio') || n.includes('mgba'));
  console.log("  sio/mgba 命中:", hit.length ? hit.join(', ') : "(无，导出名被 minify)");

  // 实际调一次 attach 看是否真工作
  console.log("\n=== 实调验证 ===");
  try {
    const create = m.cwrap('mgba_create', 'number', []);
    const attach = m.cwrap('mgba_sio_attach', 'number', ['number']);
    const core = create();
    console.log("  create core:", core);
    console.log("  attach ret:", attach(core));
  } catch (e) {
    console.log("  调用失败:", e.message);
  }

  process.exit(0);
})().catch(e => { console.error("FAIL:", e); process.exit(1); });

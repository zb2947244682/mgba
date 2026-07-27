// SIO 联机导出 smoke test（node，传 wasmBinary 绕开 web-only fetch）
const fs = require('fs');
const path = require('path');
const createModule = require('./mgba.js');

const wasmBinary = fs.readFileSync(path.join(__dirname, 'mgba.wasm'));
createModule({ wasmBinary }).then(module => {
  const cwrap = module.cwrap;
  const create = cwrap('mgba_create', 'number', []);
  const attach = cwrap('mgba_sio_attach', 'number', ['number']);
  const setPeer = cwrap('mgba_sio_set_peer', null, ['number', 'number', 'number']);
  const onPeer = cwrap('mgba_sio_on_peer', null, ['number', 'number', 'number', 'number', 'number']);
  const setMode = cwrap('mgba_sio_test_set_mode', null, ['number', 'number']);
  const detach = cwrap('mgba_sio_detach', null, ['number']);
  const reset = cwrap('mgba_reset', null, ['number']);
  const destroy = cwrap('mgba_destroy', null, ['number']);

  // __netSioSend 钩子：记录发出的帧，验证 start() 路径
  const sent = [];
  module.__netSioSend = (mode, siocnt, lo, hi, source) => sent.push([mode, siocnt, lo, hi, source]);

  const core = create();
  if (!core) throw new Error('mgba_create returned null');
  console.log('core:', core);

  if (attach(core) !== 1) throw new Error('mgba_sio_attach returned 0');
  console.log('attach: ok');
  setPeer(core, 1, 0); // 对端已连接，本机=主机(id=0)
  console.log('set_peer: ok');

  // 模拟 NORMAL32(mode=1) 双向异步模型：
  //   source=0（对端查询）且本机非 pending → 纯接收方，回发本机数据(source=1)响应 → sent 增 1。
  //   source=1（对端响应）→ 不回发（防死循环）→ sent 不增。
  reset(core);
  setMode(core, 1); // NORMAL32：on_peer 现校验 mode==sio->mode，须先设一致
  onPeer(core, 1, 0, 0x5678, 0x1234); // source=0, pending=0 → 应回发
  console.log('on_peer(NORMAL32, src=0, no pending) sent frames:', sent.length);
  if (sent.length !== 1) throw new Error('NORMAL on_peer src=0 receiver should echo once');

  sent.length = 0;
  onPeer(core, 1, 1, 0xFFFF, 0xFFFF); // source=1 响应 → 不回发
  console.log('on_peer(NORMAL32, src=1) sent:', sent.length);
  if (sent.length !== 0) throw new Error('NORMAL on_peer src=1 response must not echo');

  detach(core);
  console.log('detach: ok');
  destroy(core);
  console.log('SIO smoke test passed');
}).catch(err => {
  console.error('FAIL:', err);
  process.exit(1);
});

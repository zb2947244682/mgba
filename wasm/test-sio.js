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
  const onPeer = cwrap('mgba_sio_on_peer', null, ['number', 'number', 'number', 'number']);
  const detach = cwrap('mgba_sio_detach', null, ['number']);
  const reset = cwrap('mgba_reset', null, ['number']);
  const destroy = cwrap('mgba_destroy', null, ['number']);

  // __netSioSend 钩子：记录发出的帧，验证 start() 路径
  const sent = [];
  module.__netSioSend = (mode, siocnt, lo, hi) => sent.push([mode, siocnt, lo, hi]);

  const core = create();
  if (!core) throw new Error('mgba_create returned null');
  console.log('core:', core);

  if (attach(core) !== 1) throw new Error('mgba_sio_attach returned 0');
  console.log('attach: ok');
  setPeer(core, 1, 0); // 对端已连接，本机=主机(id=0)
  console.log('set_peer: ok');

  // 模拟一次 NORMAL32(mode=1) 对端请求：
  // pending=0 -> 走接收方路径：读本地 SIODATA32 -> __netSioSend 回发 -> FinishTransfer
  // 核心未 reset/loadRom，SIODATA 为 0；FinishTransfer 会写寄存器 + GBARaiseIRQ。
  reset(core);
  onPeer(core, 1, 0x5678, 0x1234); // peer data = 0x12345678
  console.log('on_peer(NORMAL32) sent frames:', sent.length, sent[0]);
  if (sent.length !== 1) throw new Error('receiver should have sent 1 frame');
  // 接收方回发本机 SIODATA（核心未 loadRom，SIODATA 为 0）。
  if (sent[0][2] !== 0 || sent[0][3] !== 0) throw new Error('should send local 0');

  // 再模拟一次：pending 仍 0（上一次 finish 后未 start），又是接收方路径。
  // 此时 SIODATA32 仍是上次 FinishTransfer 写入的 peer（0x5678/0x1234），因测试无游戏
  // 逻辑主动写 SIODATA；真实游戏设 Si 后会主动写自己的握手数据覆盖它。
  sent.length = 0;
  onPeer(core, 1, 0xFFFF, 0xFFFF);
  console.log('on_peer #2 sent:', sent.length, sent[0]);
  if (sent[0][2] !== 0x5678 || sent[0][3] !== 0x1234) throw new Error('should read last peer');

  detach(core);
  console.log('detach: ok');
  destroy(core);
  console.log('SIO smoke test passed');
}).catch(err => {
  console.error('FAIL:', err);
  process.exit(1);
});

// GBA 联机驱动模型级 harness（无需 ROM）。
//
// 目的：前 7 轮联机失败的根因之一是“改一轮 C → 上传 CDN → 部署 → 两浏览器人肉测”，
// 回路太慢且不可复现。本 harness 在单进程内建两个独立 wasm 实例（各自独立堆与静态
// s_netDriver），把 A 的 __netSioSend 直连 B 的 mgba_sio_on_peer（反之亦然），可选虚拟
// 延迟，逐帧/逐帧包步进，全流量日志捕获，对驱动模型的 on_peer/netStart 控制流做断言。
//
// 不加载真实游戏：用测试钩子 mgba_sio_test_start / _test_set_mode / _test_read_siodata32
// 直接驱动发起方路径与模式切换，验证 pending/source/echo/finish 逻辑。
// 真实游戏级复现（握手现场）需 ROM + 联机房间门口存档，见文末“真实 ROM 模式”。
//
// 运行：node test-netplay.js
// 可选环境：ROM_PATH + SAVE_STATE_PATH（进入真实 ROM 模式，见文件末）。

const fs = require('fs');
const path = require('path');
const createModule = require('./mgba.js');

const MGBA_N8 = 0, MGBA_N32 = 1, MGBA_MULTI = 2;

// ─── 两个独立实例 ───────────────────────────────────────────────────
const wasmBinary = fs.readFileSync(path.join(__dirname, 'mgba.wasm'));

async function makeEnd(name) {
  const lines = [];
  const sent = []; // {mode, siocnt, lo, hi, src}
  const frames = []; // 帧锁步：本端发出的帧包 {n, send, start}
  const modes = [];  // 帧锁步：本端模式通知序列
  // print/printErr 必须在 createModule 时传入：emscripten 在 init 时把 print 捕获进闭包，
  // 之后赋值 m.print 无效（__netSioSend 则是运行时读 Module，可后赋值）。
  const print = (t) => { lines.push(String(t)); };
  const printErr = (t) => { lines.push(String(t)); };
  const m = await createModule({ wasmBinary, print, printErr });
  const cwrap = m.cwrap;
  const core = cwrap('mgba_create', 'number', [])(/* 创建但不 init，避免无 ROM 报错 */);
  // 注意：mgba_create 内部已 init（GBACoreCreate + init）。无 ROM 也能 attach SIO。
  const api = {
    core,
    attach: cwrap('mgba_sio_attach', 'number', ['number']),
    setPeer: cwrap('mgba_sio_set_peer', null, ['number', 'number', 'number']),
    detach: cwrap('mgba_sio_detach', null, ['number']),
    onPeer: cwrap('mgba_sio_on_peer', null, ['number', 'number', 'number', 'number', 'number']),
    testStart: cwrap('mgba_sio_test_start', null, ['number']),
    testSetMode: cwrap('mgba_sio_test_set_mode', null, ['number', 'number']),
    testReadSiodata32: cwrap('mgba_sio_test_read_siodata32', 'number', ['number']),
    destroy: cwrap('mgba_destroy', null, ['number']),
    // ── 帧边界锁步 ──
    setLockstep: cwrap('mgba_sio_set_lockstep', null, ['number', 'number']),
    onPeerMode: cwrap('mgba_sio_on_peer_mode', null, ['number', 'number']),
    gate: cwrap('mgba_sio_gate', 'number', ['number']),
    onPeerFrame: cwrap('mgba_sio_on_peer_frame', null, ['number', 'number', 'number', 'number']),
    frameEnd: cwrap('mgba_sio_frame_end', 'number', ['number']),
    finishMulti: cwrap('mgba_sio_finish_multi', null, ['number', 'number', 'number']),
    testWriteSend: cwrap('mgba_sio_test_write_send', null, ['number', 'number']),
    testReadMultireg: cwrap('mgba_sio_test_read_multireg', 'number', ['number', 'number']),
  };
  // __netSioSend 由 C 侧 netStart / on_peer 回调注入；具体投递由 harness 互连设置。
  m.__netSioSend = (mode, siocnt, lo, hi, src) => {
    sent.push({ mode, siocnt, lo, hi, src });
  };
  // 帧锁步出向钩子默认只记录；跨端投递由 wireLockstep 设置。
  m.__netFrameSend = (n, send, start) => { frames.push({ n, send, start }); };
  m.__netModeChange = (mode) => { modes.push(mode); };
  return { name, module: m, ...api, lines, sent, frames, modes,
           reset() { lines.length = 0; sent.length = 0; frames.length = 0; modes.length = 0; } };
}

// ─── 互连：A.send → (延迟) → B.onPeer，反之亦然 ───────────────────
function wire(a, b, delayMs = 0) {
  a.module.__netSioSend = (mode, siocnt, lo, hi, src) => {
    a.sent.push({ mode, siocnt, lo, hi, src });
    const deliver = () => { try { b.onPeer(b.core, mode, src, lo, hi); } catch (e) { console.error(`[${b.name}] onPeer throw`, e); } };
    if (delayMs > 0) setTimeout(deliver, delayMs); else deliver();
  };
  b.module.__netSioSend = (mode, siocnt, lo, hi, src) => {
    b.sent.push({ mode, siocnt, lo, hi, src });
    const deliver = () => { try { a.onPeer(a.core, mode, src, lo, hi); } catch (e) { console.error(`[${a.name}] onPeer throw`, e); } };
    if (delayMs > 0) setTimeout(deliver, delayMs); else deliver();
  };
}

// 同步投递（延迟=0 时 netStart→onPeer 链是同步的，可串行断言）。
// 注意：on_peer 内若再回发（echo），会再次同步进入对端 on_peer，形成同步递归。
// 本模型 source=1 不回发，故递归有界。

// ─── 帧锁步互连：frameEnd 产的帧包同步直达对端 on_peer_frame ─────────────
function wireLockstep(a, b) {
  a.module.__netFrameSend = (n, send, start) => {
    a.frames.push({ n, send, start });
    b.onPeerFrame(b.core, n, send, start);
  };
  b.module.__netFrameSend = (n, send, start) => {
    b.frames.push({ n, send, start });
    a.onPeerFrame(a.core, n, send, start);
  };
}

// 跑一帧：模拟真实循环 gate→(runFrame)→frameEnd。返回 'ran' | 'stalled'。
function lsStep(end) {
  if (!end.gate(end.core)) return 'stalled';
  end.frameEnd(end.core);
  return 'ran';
}
// 两端各跑一帧（A 先 B 后，帧包同步互达）。
function lsStepBoth(a, b) {
  const ra = lsStep(a);
  const rb = lsStep(b);
  return [ra, rb];
}

// ─── 断言工具 ──────────────────────────────────────────────────────
let failures = 0;
function check(cond, msg) {
  const tag = cond ? 'PASS' : 'FAIL';
  console.log(`  [${tag}] ${msg}`);
  if (!cond) failures++;
}
function hasLine(end, needle) { return end.lines.some((l) => l.includes(needle)); }
function sentCount(end, srcFilter) {
  return srcFilter === undefined ? end.sent.length : end.sent.filter((s) => s.src === srcFilter).length;
}

// ─── 场景 ──────────────────────────────────────────────────────────
async function main() {
  const A = await makeEnd('A');
  const B = await makeEnd('B');
  wire(A, B, 0); // 同步互连

  // 基础 attach：A=主机(player0), B=从机(player1)
  check(A.attach(A.core) === 1, 'A attach 返回 1');
  A.setPeer(A.core, 1, 0);
  check(B.attach(B.core) === 1, 'B attach 返回 1');
  B.setPeer(B.core, 1, 1);

  // ═══ 场景 1：NORMAL32 接收方回发（src=0, !pending → echo src=1 一次） ═══
  console.log('\n=== 场景1: NORMAL32 接收方回发 ===');
  A.reset(); B.reset();
  A.testSetMode(A.core, MGBA_N32); B.testSetMode(B.core, MGBA_N32);
  // 直接向 B 注入一条“对端查询”帧（模拟 A 的 netStart 已发出 src=0 到达 B）
  B.onPeer(B.core, MGBA_N32, 0, 0x495E, 0xFFFF);
  check(sentCount(B, 1) === 1, `B 回发 1 次 src=1（got ${sentCount(B, 1)}）`);
  check(hasLine(B, '回发'), 'B 日志含“回发”');
  check(sentCount(A) === 0, `A 不应发送（got ${sentCount(A)}）`);

  // ═══ 场景 2：NORMAL32 发起方完成（test_start → B echo → A 完成） ═══
  console.log('\n=== 场景2: NORMAL32 发起方完整往返 ===');
  A.reset(); B.reset();
  A.testSetMode(A.core, MGBA_N32); B.testSetMode(B.core, MGBA_N32);
  A.testStart(A.core); // A.netStart: 发 src=0, pending=1
  // 同步链：A.send(src=0) → B.onPeer(src=0,!pending) → B echo(src=1) → A.onPeer(src=1,pending) → 完成
  check(sentCount(A, 0) === 1, `A 发出 1 次 src=0（got ${sentCount(A, 0)}）`);
  check(sentCount(B, 1) === 1, `B 回发 1 次 src=1（got ${sentCount(B, 1)}）`);
  check(hasLine(A, '发起方'), 'A 日志含“发起方：收到响应”');
  check(sentCount(A, 1) === 0, `A 不应再回发 src=1（got ${sentCount(A, 1)}）`);

  // ═══ 场景 3：bug1 — 迟到/重复 src=1 到非 pending 方，不应 finishByMode ═══
  console.log('\n=== 场景3: bug1 迟到 src=1 不应 finish ===');
  A.reset(); B.reset();
  A.testSetMode(A.core, MGBA_N32);
  // A 未 pending（没 test_start），注入一条迟到响应 src=1
  A.onPeer(A.core, MGBA_N32, 1, 0xDEAD, 0xBEEF);
  // 当前 bug：on_peer 无条件 finishByMode → 写 SIODATA + IRQ。正确行为：丢弃。
  const finished = hasLine(A, 'finishByMode') || hasLine(A, '→ finishByMode');
  check(!finished, `A 不应对迟到 src=1 调 finishByMode（当前 bug 会调，got finished=${finished}）`);
  check(sentCount(A) === 0, `A 不应回发（got ${sentCount(A)}）`);

  // ═══ 场景 4：bug2 — MULTI 从机收到迟到 src=1 不应回发（防回声风暴） ═══
  console.log('\n=== 场景4: bug2 MULTI 从机迟到 src=1 不应回发 ===');
  A.reset(); B.reset();
  A.testSetMode(A.core, MGBA_MULTI); B.testSetMode(B.core, MGBA_MULTI);
  // B 是从机(player1)，未 pending。注入迟到 src=1（本应是主机→从机的回执，但从机不该收 src=1）
  B.onPeer(B.core, MGBA_MULTI, 1, 0x1234, 0);
  check(sentCount(B) === 0, `MULTI 从机不应回发迟到 src=1（当前 bug 会回发，got ${sentCount(B)}）`);

  // ═══ 场景 5：MULTI 正常往返（主机 test_start → 从机 echo → 主机完成） ═══
  console.log('\n=== 场景5: MULTI 正常往返 ===');
  A.reset(); B.reset();
  A.testSetMode(A.core, MGBA_MULTI); B.testSetMode(B.core, MGBA_MULTI);
  A.testStart(A.core); // 主机 netStart: src=0, pending=1, lastSend 记录
  check(sentCount(A, 0) === 1, `主机发出 1 次 src=0（got ${sentCount(A, 0)}）`);
  check(sentCount(B, 1) === 1, `从机回发 1 次 src=1（got ${sentCount(B, 1)}）`);
  check(hasLine(A, '主机 on_peer'), 'A 日志含“MULTI 主机 on_peer”');
  check(sentCount(A, 1) === 0, `主机不应再回发（got ${sentCount(A, 1)}）`);

  // ═══ 场景 6：bug3 — on_peer 不校验 mode，异模式帧应丢弃 ═══
  console.log('\n=== 场景6: bug3 异模式帧不应处理 ===');
  A.reset(); B.reset();
  A.testSetMode(A.core, MGBA_N32); // 本机 NORMAL32
  // 注入一条 MULTI 帧（mode 不符）
  A.onPeer(A.core, MGBA_MULTI, 0, 0x1111, 0x2222);
  const handled = hasLine(A, 'finishByMode') || hasLine(A, '→ finishByMode') || hasLine(A, '回发') || hasLine(A, '主机');
  check(!handled, `A 不应处理异模式 MULTI 帧（当前 bug 会处理，got handled=${handled}）`);

  // ═══════════════════════════════════════════════════════════════
  // 帧边界锁步场景（PLAN §6.3 规则断言）。S=2。
  // ═══════════════════════════════════════════════════════════════
  wireLockstep(A, B);
  A.setLockstep(A.core, 2);
  B.setLockstep(B.core, 2);

  // ═══ 场景 7：未激活时 start 以 0xFFFF 兜底完成（对端未进 MULTI） ═══
  console.log('\n=== 场景7: 未激活 start 以 0xFFFF 兜底完成 ===');
  A.reset(); B.reset();
  A.testSetMode(A.core, MGBA_MULTI); // 仅 A 进 MULTI，remoteMulti=0 → 未激活
  check(A.gate(A.core) === 1, '未激活时 gate 恒放行');
  A.testWriteSend(A.core, 0x1234);
  A.testStart(A.core);          // 锁步路：只记录不发网
  check(sentCount(A) === 0, `锁步路 netStart 不发 sio 网络帧（got ${sentCount(A)}）`);
  A.frameEnd(A.core);
  check(A.testReadMultireg(A.core, 0) === 0x1234, `主机读回自己的 send（got 0x${A.testReadMultireg(A.core, 0).toString(16)}）`);
  check(A.testReadMultireg(A.core, 1) === 0xFFFF, `空挂槽位 0xFFFF（got 0x${A.testReadMultireg(A.core, 1).toString(16)}）`);
  check(hasLine(A, '未激活'), 'A 日志含“未激活”兜底');
  check(A.frames.length === 0, `未激活不发帧包（got ${A.frames.length}）`);

  // ═══ 场景 8：激活协商（双端报 MULTI 后帧号归 0 开始门控） ═══
  console.log('\n=== 场景8: 激活协商 ===');
  A.reset(); B.reset();
  B.testSetMode(B.core, MGBA_MULTI);
  check(!hasLine(A, 'ls ACTIVATE') && !hasLine(B, 'ls ACTIVATE'), '双端未互报前不激活');
  A.onPeerMode(A.core, 1);
  B.onPeerMode(B.core, 1);
  check(hasLine(A, 'ls ACTIVATE'), 'A 激活');
  check(hasLine(B, 'ls ACTIVATE'), 'B 激活');
  check(B.modes.includes(MGBA_MULTI), 'B 模式通知已发出（前端据此发 mb）');

  // ═══ 场景 9：门控停顿与恢复（S=2：A 最多领先 B 两帧） ═══
  console.log('\n=== 场景9: 门控停顿与恢复 ===');
  A.reset(); B.reset();
  lsStepBoth(A, B); // 帧 0
  lsStepBoth(A, B); // 帧 1
  // B 暂停（模拟对端卡帧/切后台），A 最多再领先 S=2 帧
  check(lsStep(A) === 'ran', 'A 帧 2 跑（有 B 包 0）');
  check(lsStep(A) === 'ran', 'A 帧 3 跑（有 B 包 1）');
  check(lsStep(A) === 'stalled', 'A 帧 4 被门控（B 停在帧 1，缺包 2）');
  check(lsStep(A) === 'stalled', '补跑前持续停顿');
  check(lsStep(B) === 'ran', 'B 恢复跑帧 2');
  check(lsStep(A) === 'ran', 'A 帧 4 放行');

  // ═══ 场景 10：S 帧延迟传输配对（核心场景） ═══
  // A=主机帧 0 start(0xAAAA)，B=从机帧 0 arm 0xBBBB；两端帧 2 边界各自完成，
  // 两端 SIOMULTI0/1 必须一致：{0xAAAA, 0xBBBB}。
  console.log('\n=== 场景10: S 帧延迟传输配对 ===');
  A.detach(A.core); B.detach(B.core);
  A.attach(A.core); B.attach(B.core);
  A.setPeer(A.core, 1, 0); B.setPeer(B.core, 1, 1);
  A.setLockstep(A.core, 2); B.setLockstep(B.core, 2);
  A.reset(); B.reset();
  A.testSetMode(A.core, MGBA_MULTI); B.testSetMode(B.core, MGBA_MULTI);
  A.onPeerMode(A.core, 1); B.onPeerMode(B.core, 1);
  A.testWriteSend(A.core, 0xAAAA);
  B.testWriteSend(B.core, 0xBBBB);
  A.testStart(A.core); // 帧 0：主机 start
  check(lsStep(A) === 'ran' && lsStep(B) === 'ran', '帧 0 双端跑');
  check(A.frames[0] && A.frames[0].start === 1 && A.frames[0].send === 0xAAAA,
        `A 帧包 0 携带 start=1 send=0xAAAA（got ${JSON.stringify(A.frames[0])}）`);
  check(B.frames[0] && B.frames[0].start === 0 && B.frames[0].send === 0xBBBB,
        `B 帧包 0 携带 start=0 send=0xBBBB（got ${JSON.stringify(B.frames[0])}）`);
  check(A.testReadMultireg(A.core, 0) !== 0xAAAA, '帧 0 边界尚未完成（S=2 延迟）');
  lsStepBoth(A, B); // 帧 1
  lsStepBoth(A, B); // 帧 2：完成 k=0
  check(A.testReadMultireg(A.core, 0) === 0xAAAA && A.testReadMultireg(A.core, 1) === 0xBBBB,
        `A 端 SIOMULTI={0xAAAA,0xBBBB}（got 0x${A.testReadMultireg(A.core, 0).toString(16)},0x${A.testReadMultireg(A.core, 1).toString(16)}）`);
  check(B.testReadMultireg(B.core, 0) === 0xAAAA && B.testReadMultireg(B.core, 1) === 0xBBBB,
        `B 端 SIOMULTI 与 A 一致（got 0x${B.testReadMultireg(B.core, 0).toString(16)},0x${B.testReadMultireg(B.core, 1).toString(16)}）`);
  check(hasLine(A, '帧边界完成') && hasLine(B, '帧边界完成'), '双端日志含“帧边界完成”');

  // ═══ 场景 11：连续流水线（帧 3 再 start → 帧 5 完成，吞吐每帧一笔） ═══
  console.log('\n=== 场景11: 连续流水线 ===');
  A.reset(); B.reset();
  A.testWriteSend(A.core, 0xCCCC);
  B.testWriteSend(B.core, 0xDDDD);
  A.testStart(A.core); // 帧 3 start
  lsStepBoth(A, B); // 帧 3
  lsStepBoth(A, B); // 帧 4
  check(A.testReadMultireg(A.core, 0) !== 0xCCCC, '帧 4 边界尚未完成第二笔');
  lsStepBoth(A, B); // 帧 5：完成 k=3
  check(A.testReadMultireg(A.core, 0) === 0xCCCC && A.testReadMultireg(A.core, 1) === 0xDDDD,
        `A 端第二笔 {0xCCCC,0xDDDD}（got 0x${A.testReadMultireg(A.core, 0).toString(16)},0x${A.testReadMultireg(A.core, 1).toString(16)}）`);
  check(B.testReadMultireg(B.core, 0) === 0xCCCC && B.testReadMultireg(B.core, 1) === 0xDDDD,
        `B 端第二笔与 A 一致`);

  // ═══ 场景 12：退出 MULTI 复位 + 重进重新激活 ═══
  console.log('\n=== 场景12: 模式进出复位 ===');
  A.reset(); B.reset();
  A.testSetMode(A.core, MGBA_N32);
  check(hasLine(A, 'ls DEACTIVATE'), 'A 退出 MULTI 停用锁步');
  check(A.gate(A.core) === 1, '停用后 gate 放行');
  check(A.frameEnd(A.core) === 0, '非 MULTI frame_end 无操作');
  A.testSetMode(A.core, MGBA_MULTI);
  check(hasLine(A, 'ls ACTIVATE'), 'A 重进 MULTI 重新激活（帧号归 0）');

  // 清理
  A.destroy(A.core); B.destroy(B.core);

  console.log(`\n=== 结果：${failures === 0 ? '全部 PASS' : failures + ' 项 FAIL'} ===`);
  if (failures > 0) {
    console.log('注：仍有 FAIL 说明驱动模型仍有 bug，按场景定位修复。');
  }
  // 用 exitCode 自然退出：process.exit 会打断 emscripten 的异步清理，
  // 在 Windows 上触发 libuv UV_HANDLE_CLOSING 断言（测试本身已通过）。
  process.exitCode = failures > 0 ? 1 : 0;
}

// ─── 真实 ROM 模式（需 ROM + 存档，本机暂无 ROM，留作后续） ─────────
// 若提供 ROM_PATH（及可选 SAVE_STATE_PATH=联机房间门口的 .ss0/.gmss0 即时存档）：
//   两端各加载 ROM + 存档 → 落在握手现场 → 逐帧步进 runFrame → 观察真实 SIO 流。
// 该模式可复现 0x495E 握手卡死，是帧锁步开发的真实数据源。
//   node test-netplay.js           → 模型级（本文）
//   ROM_PATH=... SAVE_STATE_PATH=... node test-netplay.js → 真实 ROM 级（待实现）

main().catch((e) => { console.error('harness 崩溃:', e); process.exit(2); });

/* mGBA WebAssembly bridge
 * Exposes a minimal C API for running GBA emulation in a browser.
 */
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba-util/vfs.h>
#include <mgba-util/audio-buffer.h>
#include <mgba/internal/gba/input.h>

EMSCRIPTEN_KEEPALIVE struct mCore* mgba_create() {
    struct mCore* core = GBACoreCreate();
    if (!core) return NULL;
    mCoreInitConfig(core, "wasm");
    if (!core->init(core)) {
        core->deinit(core);
        return NULL;
    }
    return core;
}

EMSCRIPTEN_KEEPALIVE struct mCore* mgba_create_raw() {
    struct mCore* core = GBACoreCreate();
    if (core) {
        mCoreInitConfig(core, "wasm");
    }
    return core;
}

EMSCRIPTEN_KEEPALIVE int mgba_init_core(struct mCore* core) {
    if (!core) return 0;
    return core->init(core) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int mgba_test_full_init() {
    printf("Step 1: calling GBACoreCreate\n");
    fflush(stdout);
    struct mCore* core = GBACoreCreate();
    if (!core) {
        printf("Step 1 failed: GBACoreCreate returned NULL\n");
        fflush(stdout);
        return 0;
    }
    printf("Step 2: calling core->init\n");
    fflush(stdout);
    int ok = core->init(core) ? 1 : 0;
    if (!ok) {
        printf("Step 2 failed: core->init returned false\n");
        fflush(stdout);
        return 1;
    }
    printf("Step 3: deinit\n");
    fflush(stdout);
    core->deinit(core);
    printf("Success\n");
    fflush(stdout);
    return 2;
}

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/arm/arm.h>

EMSCRIPTEN_KEEPALIVE int mgba_test_gba_create() {
    printf("Testing GBACreate...\n");
    fflush(stdout);
    struct GBA* gba = calloc(1, sizeof(struct GBA));
    printf("Calling GBACreate...\n");
    fflush(stdout);
    GBACreate(gba);
    printf("GBACreate done, returning\n");
    fflush(stdout);
    return 1;
}

EMSCRIPTEN_KEEPALIVE int mgba_test_arm_init() {
    printf("Testing ARMInit...\n");
    fflush(stdout);
    struct ARMCore* cpu = calloc(1, sizeof(struct ARMCore));
    struct GBA* gba = calloc(1, sizeof(struct GBA));
    printf("Calling GBACreate...\n");
    fflush(stdout);
    GBACreate(gba);
    printf("Calling ARMSetComponents...\n");
    fflush(stdout);
    struct mCPUComponent* components[CPU_COMPONENT_MAX] = {0};
    ARMSetComponents(cpu, &gba->d, CPU_COMPONENT_MAX, components);
    printf("Calling ARMInit...\n");
    fflush(stdout);
    ARMInit(cpu);
    printf("ARMInit done\n");
    fflush(stdout);
    ARMDeinit(cpu);
    free(cpu);
    free(gba);
    return 1;
}

EMSCRIPTEN_KEEPALIVE void mgba_destroy(struct mCore* core) {
    if (core) {
        core->deinit(core);
    }
}

EMSCRIPTEN_KEEPALIVE int mgba_load_rom(struct mCore* core, const void* data, size_t size) {
    if (!core || !data || !size) return 0;
    struct VFile* vf = VFileMemChunk(data, size);
    if (!vf) return 0;
    int ok = core->loadROM(core, vf) ? 1 : 0;
    // Keep vf open; the core owns it and will close it when deinitialized.
    return ok;
}

EMSCRIPTEN_KEEPALIVE void mgba_reset(struct mCore* core) {
    if (core) core->reset(core);
}

// Configuration helpers (must be called before mgba_init_core)
EMSCRIPTEN_KEEPALIVE void mgba_set_config_int(struct mCore* core, const char* key, int value) {
    if (core && key) {
        mCoreConfigSetIntValue(&core->config, key, value);
    }
}

EMSCRIPTEN_KEEPALIVE void mgba_set_config_float(struct mCore* core, const char* key, float value) {
    if (core && key) {
        mCoreConfigSetFloatValue(&core->config, key, value);
    }
}

EMSCRIPTEN_KEEPALIVE void mgba_set_config_string(struct mCore* core, const char* key, const char* value) {
    if (core && key && value) {
        mCoreConfigSetValue(&core->config, key, value);
    }
}

// SRAM / FLASH savedata helpers
EMSCRIPTEN_KEEPALIVE size_t mgba_get_save_size(struct mCore* core) {
    if (!core) return 0;
    void* sram = NULL;
    size_t size = core->savedataClone(core, &sram);
    if (sram) free(sram);
    return size;
}

EMSCRIPTEN_KEEPALIVE size_t mgba_read_save(struct mCore* core, void* buffer, size_t size) {
    if (!core || !buffer || !size) return 0;
    void* sram = NULL;
    size_t actual = core->savedataClone(core, &sram);
    if (!actual || !sram) return 0;
    size_t copy = actual < size ? actual : size;
    memcpy(buffer, sram, copy);
    free(sram);
    return copy;
}

EMSCRIPTEN_KEEPALIVE int mgba_write_save(struct mCore* core, const void* data, size_t size) {
    if (!core || !data || !size) return 0;
    return core->savedataRestore(core, data, size, true) ? 1 : 0;
}

// 正确的存档加载入口：用 VFileMemChunk 包装字节流并交给 core->loadSave，
// 这会把 savedata.vf 设为该 VFile。游戏运行后首次写存档触发类型检测时，
// GBASavedataInit{Flash,SRAM,EEPROM} 会把 vf 映射进 data，游戏即可读到旧存档。
// 对 SRAM / FLASH512 / FLASH1M / EEPROM 全类型均有效（类型由游戏运行时检测，不强制）。
// 必须在首次 runFrame 之前调用（loadROM 之后、reset 之前或之后均可，reset 不会清掉 vf）。
EMSCRIPTEN_KEEPALIVE int mgba_load_save(struct mCore* core, const void* data, size_t size) {
    if (!core || !data || !size) return 0;
    struct VFile* vf = VFileMemChunk(data, size);
    if (!vf) return 0;
    return core->loadSave(core, vf) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void mgba_run_frame(struct mCore* core) {
    if (core) core->runFrame(core);
}

EMSCRIPTEN_KEEPALIVE unsigned mgba_get_video_width(struct mCore* core) {
    unsigned w = 0, h = 0;
    if (core) core->baseVideoSize(core, &w, &h);
    return w;
}

EMSCRIPTEN_KEEPALIVE unsigned mgba_get_video_height(struct mCore* core) {
    unsigned w = 0, h = 0;
    if (core) core->baseVideoSize(core, &w, &h);
    return h;
}

EMSCRIPTEN_KEEPALIVE void mgba_set_video_buffer(struct mCore* core, void* buffer, size_t stride) {
    if (core) core->setVideoBuffer(core, buffer, stride);
}

EMSCRIPTEN_KEEPALIVE unsigned mgba_get_audio_sample_rate(struct mCore* core) {
    if (!core) return 0;
    return core->audioSampleRate(core);
}

EMSCRIPTEN_KEEPALIVE size_t mgba_read_audio(struct mCore* core, int16_t* samples, size_t count) {
    if (!core || !samples) return 0;
    struct mAudioBuffer* buffer = core->getAudioBuffer(core);
    if (!buffer) return 0;
    return mAudioBufferRead(buffer, samples, count);
}

EMSCRIPTEN_KEEPALIVE void mgba_set_keys(struct mCore* core, uint32_t keys) {
    if (core) core->setKeys(core, keys);
}

EMSCRIPTEN_KEEPALIVE void mgba_clear_keys(struct mCore* core, uint32_t keys) {
    if (core) core->clearKeys(core, keys);
}

EMSCRIPTEN_KEEPALIVE size_t mgba_state_size(struct mCore* core) {
    if (!core) return 0;
    return core->stateSize(core);
}

EMSCRIPTEN_KEEPALIVE int mgba_save_state(struct mCore* core, void* buffer) {
    if (!core || !buffer) return 0;
    return core->saveState(core, buffer) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int mgba_load_state(struct mCore* core, const void* buffer) {
    if (!core || !buffer) return 0;
    return core->loadState(core, buffer) ? 1 : 0;
}

/* Key constants re-exported for JS side */
EMSCRIPTEN_KEEPALIVE int mgba_key_a() { return GBA_KEY_A; }
EMSCRIPTEN_KEEPALIVE int mgba_key_b() { return GBA_KEY_B; }
EMSCRIPTEN_KEEPALIVE int mgba_key_select() { return GBA_KEY_SELECT; }
EMSCRIPTEN_KEEPALIVE int mgba_key_start() { return GBA_KEY_START; }
EMSCRIPTEN_KEEPALIVE int mgba_key_right() { return GBA_KEY_RIGHT; }
EMSCRIPTEN_KEEPALIVE int mgba_key_left() { return GBA_KEY_LEFT; }
EMSCRIPTEN_KEEPALIVE int mgba_key_up() { return GBA_KEY_UP; }
EMSCRIPTEN_KEEPALIVE int mgba_key_down() { return GBA_KEY_DOWN; }
EMSCRIPTEN_KEEPALIVE int mgba_key_r() { return GBA_KEY_R; }
EMSCRIPTEN_KEEPALIVE int mgba_key_l() { return GBA_KEY_L; }

/* ==================== SIO 网络联机 ====================
 * 自定义 GBASIODriver：把 SIO 传输通过 WebSocket 转发给对端浏览器。
 * 设计：对称 request-response 握手。
 *   - start() 读本机待发数据，通过 EM_JS 钩子发给对端，返回 false（异步完成），
 *     核心 _startTransfer 见返回 false 即不调度 completeEvent（sio.c:143-149）。
 *   - 对端数据到达时由 mgba_sio_on_peer 调 GBASIO*FinishTransfer 完成传输
 *     （写 SIODATA + 清 Start + 触发 SIO 中断，sio.c:391-406）。
 *   - pending 标志区分：发起方收到的是"回执"（直接 finish）；
 *     接收方收到的是"请求"（回发本机数据 + finish）。
 */
#include <mgba/gba/interface.h>      /* GBASIODriver, mPERIPH_GBA_LINK_PORT */
#include <mgba/internal/gba/sio.h>    /* GBASIO, GBASIO*FinishTransfer */
#include <mgba/internal/gba/io.h>     /* GBA_REG, SIODATA* / SIOMLT_SEND */

/* 前端注入的发送钩子。data_lo/data_hi 携带本机待发数据（按 mode 解析）。
 * source: 0=netStart（本机主动 start 发送），1=on_peer 接收方回发（收到对端请求后回发本机数据）。 */
EM_JS(void, mgba_net_send, (int mode, int siocnt, unsigned data_lo, unsigned data_hi, int source), {
    if (typeof Module.__netSioSend === 'function') {
        Module.__netSioSend(mode, siocnt, data_lo, data_hi, source);
    }
});

struct GBASIONetDriver {
    struct GBASIODriver d;
    int connected;       /* 对端是否已连接 */
    int playerId;        /* MULTI 本机 id（0=主机,1=从机） */
    int pending;         /* 本机已发起 start（NORMAL8/32/MULTI），等待对端响应才完成 */
    unsigned lastSend;   /* MULTI 主机 finish 时回填 data[0] */
};

static struct GBASIONetDriver s_netDriver;

static unsigned readLocalSend(struct GBASIO* sio, int mode) {
    switch (mode) {
    case GBA_SIO_NORMAL_8:
        return sio->p->memory.io[GBA_REG(SIODATA8)] & 0xFF;
    case GBA_SIO_NORMAL_32:
        return (sio->p->memory.io[GBA_REG(SIODATA32_LO)] & 0xFFFF)
             | ((sio->p->memory.io[GBA_REG(SIODATA32_HI)] & 0xFFFF) << 16);
    case GBA_SIO_MULTI:
        return sio->p->memory.io[GBA_REG(SIOMLT_SEND)] & 0xFFFF;
    default:
        return 0;
    }
}

static void finishByMode(struct GBASIO* sio, int mode, unsigned peer, unsigned my) {
    switch (mode) {
    case GBA_SIO_NORMAL_8:
        GBASIONormal8FinishTransfer(sio, (uint8_t)(peer & 0xFF), 0);
        break;
    case GBA_SIO_NORMAL_32:
        GBASIONormal32FinishTransfer(sio, peer, 0);
        break;
    case GBA_SIO_MULTI: {
        /* data[0]=主机发送, data[1]=从机发送，2P 其余填 0xFFFF */
        uint16_t data[4];
        if (s_netDriver.playerId == 0) {
            data[0] = (uint16_t)(my & 0xFFFF);
            data[1] = (uint16_t)(peer & 0xFFFF);
        } else {
            data[0] = (uint16_t)(peer & 0xFFFF);
            data[1] = (uint16_t)(my & 0xFFFF);
        }
        data[2] = 0xFFFF;
        data[3] = 0xFFFF;
        GBASIOMultiplayerFinishTransfer(sio, data, 0);
        break;
    }
    default:
        break;
    }
}

/* SIO mode 枚举名（与 sio.h GBASIOMode 一致：0=NORMAL8 1=NORMAL32 2=MULTI 3=UART），便于 printf 可读。 */
static const char* sioModeStr(int m) {
    switch (m) {
        case 0: return "NORMAL8";
        case 1: return "NORMAL32";
        case 2: return "MULTI";
        case 3: return "UART";
        default: return "?";
    }
}
static bool netInit(struct GBASIODriver* d) { (void)d; return true; }
static void netDeinit(struct GBASIODriver* d) { (void)d; }
static void netReset(struct GBASIODriver* d) {
    struct GBASIONetDriver* n = (struct GBASIONetDriver*)d;
    n->pending = 0;
    n->lastSend = 0;
}
static uint32_t netId(const struct GBASIODriver* d) { (void)d; return 0x574F524E; /* 'NETW' */ }
static bool netLoadState(struct GBASIODriver* d, const void* s, size_t sz) { (void)d; (void)s; (void)sz; return false; }
static void netSaveState(struct GBASIODriver* d, void** s, size_t* sz) { (void)d; if (s) *s = NULL; if (sz) *sz = 0; }
static void netSetMode(struct GBASIODriver* d, enum GBASIOMode mode) {
    (void)d;
    printf("[sio] setMode mode=%d(%s)\n", (int)mode, sioModeStr((int)mode));
    fflush(stdout);
}
static bool netHandlesMode(struct GBASIODriver* d, enum GBASIOMode mode) {
    (void)d;
    bool r = mode == GBA_SIO_NORMAL_8 || mode == GBA_SIO_NORMAL_32 || mode == GBA_SIO_MULTI;
    /* 去重：mGBA 可能逐帧询问，只在 mode 变化时打印。返回值不变所以去重安全。 */
    static int s_lastHandled = -1;
    int m = (int)mode;
    if (m != s_lastHandled) {
        printf("[sio] handlesMode mode=%d(%s) → %d\n", m, sioModeStr(m), r ? 1 : 0);
        fflush(stdout);
        s_lastHandled = m;
    }
    return r;
}
static int netConnected(struct GBASIODriver* d) {
    return ((struct GBASIONetDriver*)d)->connected ? 1 : 0;
}
static int netDeviceId(struct GBASIODriver* d) {
    return ((struct GBASIONetDriver*)d)->playerId;
}
static uint16_t netWriteSIOCNT(struct GBASIODriver* d, uint16_t v) {
    /* MULTI 就绪位：游戏轮询 SIOCNT.Ready 判断所有玩家是否进入同一 MULTI mode，
     * Ready=0 时游戏不会 start 传输（实测：日志只有 attach 无 sio send）。
     * 参照 lockstep _setReady（sio/lockstep.c:856-860）：双方 attach 且 MULTI 时 ready=1。
     * v 会被 sio.c:238 存入 sio->siocnt，故 Ready 设在 v；SD 位在 RCNT，直接设 sio->rcnt。
     *
     * NORMAL8/32 的 Si 位（bit 2，sio.h:42）：游戏读它判断对端 SO 是否为高 = 对端是否在线/就绪。
     * 虚拟驱动在 sio.c:228 调 GBASIONormalFillSi 设 Si=1，但我们 handled=true 会跳过那行，
     * 导致 Si=0，游戏判定无对端、从机不写自己的握手数据 → 握手失败。故此处同样设 Si=1。 */
    struct GBASIO* sio = d->p;
    /* 去重：游戏轮询 SIOCNT 时可能逐帧写，只在值变化时打印，避免刷屏。
     * 关键看 mode 落点 + Start 位(bit7) + Ready 位 + connected/pending 状态。 */
    static uint16_t s_lastSiocnt = 0xFFFF;
    if (v != s_lastSiocnt) {
        printf("[sio] writeSIOCNT v=0x%04X mode=%d(%s) connected=%d pending=%d\n",
               (unsigned)v & 0xFFFF, (int)sio->mode, sioModeStr((int)sio->mode),
               s_netDriver.connected, s_netDriver.pending);
        fflush(stdout);
        s_lastSiocnt = v;
    }
    if (!s_netDriver.connected) return v;
    if (sio->mode == GBA_SIO_MULTI) {
        v = GBASIOMultiplayerSetReady(v, 1);
        sio->rcnt = GBASIORegisterRCNTSetSd(sio->rcnt, 1);
    } else if (sio->mode == GBA_SIO_NORMAL_8 || sio->mode == GBA_SIO_NORMAL_32) {
        v = GBASIONormalFillSi(v);
    }
    return v;
}
static uint16_t netWriteRCNT(struct GBASIODriver* d, uint16_t v) { (void)d; return v; }

/* NORMAL8/32/MULTI 均异步：netStart 发本机数据(source=0)并置 pending，return false 不调度
 * completeEvent。本机传输在对端数据到达（on_peer）时由 finishByMode 完成 + IRQ。
 * 关键：这是双向模型——发起方必须收到对端响应才算完成（无线接收器协议要求双向数据，
 * 不能像 mGBA lockstep 那样主机返回 0xFFFFFFFF 单向）。游戏若 HALT 等 SIO IRQ，则
 * 异步等待期间无仿真推进、无超时；on_peer 到达即 IRQ 唤醒。 */
static bool netStart(struct GBASIODriver* d) {
    struct GBASIONetDriver* n = (struct GBASIONetDriver*)d;
    struct GBASIO* sio = d->p;
    int mode = (int)sio->mode;
    unsigned send = readLocalSend(sio, mode);
    /* 诊断：mode: 0=NORMAL8 1=NORMAL32 2=MULTI 3=UART。playerId: 0=主机 1=从机。
     * 若 netStart 从不触发，说明游戏未进入 SIO 传输（mode/Ready/Si 位未就绪）。 */
    printf("[sio] netStart mode=%d player=%d pending=%d connected=%d siocnt=0x%04X send=0x%04X\n",
           mode, n->playerId, n->pending, n->connected, (unsigned)sio->siocnt & 0xFFFF, send & 0xFFFF);
    fflush(stdout);
    mgba_net_send(mode, sio->siocnt, send & 0xFFFF, (send >> 16) & 0xFFFF, 0); /* source=0: 本机查询 */
    n->pending = 1;
    if (mode == GBA_SIO_MULTI) {
        n->lastSend = send;
    }
    return false;
}

EMSCRIPTEN_KEEPALIVE int mgba_sio_attach(struct mCore* core) {
    if (!core || !core->board) return 0;
    struct GBASIODriver* d = &s_netDriver.d;
    d->init = netInit; d->deinit = netDeinit; d->reset = netReset;
    d->driverId = netId; d->loadState = netLoadState; d->saveState = netSaveState;
    d->setMode = netSetMode; d->handlesMode = netHandlesMode;
    d->connectedDevices = netConnected; d->deviceId = netDeviceId;
    d->writeSIOCNT = netWriteSIOCNT; d->writeRCNT = netWriteRCNT;
    d->start = netStart;
    /* NORMAL8/32/MULTI 全异步：完成走 on_peer 的 finishByMode，核心 _sioFinish 不参与，故 finish* 全 NULL。 */
    d->finishMultiplayer = NULL; d->finishNormal8 = NULL; d->finishNormal32 = NULL;
    s_netDriver.connected = 0; s_netDriver.playerId = 0;
    s_netDriver.pending = 0; s_netDriver.lastSend = 0;
    core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, d);
    printf("[sio] attach ok (driver 已注册到 LINK_PORT)\n");
    fflush(stdout);
    return 1;
}

EMSCRIPTEN_KEEPALIVE void mgba_sio_set_peer(struct mCore* core, int connected, int playerId) {
    s_netDriver.connected = connected ? 1 : 0;
    s_netDriver.playerId = playerId;
    printf("[sio] set_peer connected=%d playerId=%d\n", s_netDriver.connected, s_netDriver.playerId);
    fflush(stdout);
    /* attach 后若游戏已在 MULTI/NORMAL mode，主动设就绪位（兜底：游戏可能不再 writeSIOCNT）。
     * 与 netWriteSIOCNT 同逻辑。NORMAL 设 Si 位让游戏判定对端在线、从机才会写握手数据。 */
    if (connected && core && core->board) {
        struct GBA* gba = core->board;
        struct GBASIO* sio = &gba->sio;
        if (sio->mode == GBA_SIO_MULTI) {
            sio->siocnt = GBASIOMultiplayerSetReady(sio->siocnt, 1);
            sio->rcnt = GBASIORegisterRCNTSetSd(sio->rcnt, 1);
        } else if (sio->mode == GBA_SIO_NORMAL_8 || sio->mode == GBA_SIO_NORMAL_32) {
            sio->siocnt = GBASIONormalFillSi(sio->siocnt);
        }
    }
}

EMSCRIPTEN_KEEPALIVE void mgba_sio_on_peer(struct mCore* core, int mode, int source, unsigned data_lo, unsigned data_hi) {
    if (!core || !core->board) return;
    struct GBA* gba = core->board;
    struct GBASIO* sio = &gba->sio;
    if ((struct GBASIODriver*)&s_netDriver != sio->driver) {
        printf("[sio] on_peer DROPPED: driver not attached\n");
        fflush(stdout);
        return; /* 未 attach */
    }
    unsigned peer = (data_lo & 0xFFFF) | ((data_hi & 0xFFFF) << 16);
    /* 诊断：source: 0=对端查询(本机应回发) 1=对端响应(本机完成)。pending: 本机是否在等。
     * 正常交替应见：一端 netStart(source=0) → 对端 on_peer src=0 回发 → 本端 on_peer src=1 完成。 */
    printf("[sio] on_peer mode=%d source=%d player=%d pending=%d peer=0x%04X\n",
           mode, source, s_netDriver.playerId, s_netDriver.pending, peer & 0xFFFF);
    fflush(stdout);

    if (mode == GBA_SIO_NORMAL_8 || mode == GBA_SIO_NORMAL_32) {
        /* 双向异步 + source 防死循环。无线接收器协议是双向的（主机须收到从机响应），
         * 不能照搬 mGBA lockstep（主机 finishNormal32 返回 0xFFFFFFFF 单向，主机永不拿从机数据）。
         *
         * 模型：发起方 netStart 发 source=0 查询、置 pending、return false 等 on_peer 完成。
         *   接收方 on_peer 收 source=0：先读 my（finishByMode 会覆盖 SIODATA），再 finishByMode
         *     写对端数据 + IRQ（本机处理对端查询），最后回发 my（source=1 响应）。
         *   发起方 on_peer 收 source=1：finishByMode 写对端响应 + IRQ，清 pending，不回发。
         *   全双工（两端同时 start，都 pending）：各自收 source=0 → finishByMode + 清 pending，
         *     不回发；两端都用对端查询数据完成，无循环（beacon 型握手走此路径）。
         *   source=1 永不回发 → 重复/迟到响应不会触发再回发，杜绝死循环。 */
        int willEcho = (source == 0 && !s_netDriver.pending);
        unsigned my = willEcho ? readLocalSend(sio, mode) : 0; /* finishByMode 会覆盖 SIODATA，须先读 */
        printf("[sio] NORMAL on_peer src=%d willEcho=%d pending=%d → finishByMode(peer=0x%04X) + IRQ\n",
               source, willEcho, s_netDriver.pending, peer & 0xFFFF);
        fflush(stdout);
        finishByMode(sio, mode, peer, 0); /* 写对端数据到 SIODATA + IRQ */
        if (source == 0) {
            if (s_netDriver.pending) {
                s_netDriver.pending = 0; /* 全双工：本机也在等，用此查询完成 */
                printf("[sio]   全双工：用对端查询完成本机传输（不回发）\n");
            } else {
                printf("[sio]   接收方：回发本机数据 my=0x%04X (src=1)\n", my & 0xFFFF);
                mgba_net_send(mode, sio->siocnt, my & 0xFFFF, (my >> 16) & 0xFFFF, 1); /* source=1: 回发本机数据 */
            }
        } else if (s_netDriver.pending) {
            s_netDriver.pending = 0; /* 我的查询有响应了 */
            printf("[sio]   发起方：收到响应，完成本机传输（清 pending，不回发）\n");
        }
        return;
    }

    /* MULTI：只主机 start（sio.c:194），从机 on_peer 回发本机 SIOMLT_SEND 给主机。
     * pending 区分主机 on_peer（收到从机回执，完成）与从机 on_peer（收到主机请求，回发+完成）。 */
    if (s_netDriver.pending) {
        s_netDriver.pending = 0;
        printf("[sio] MULTI 主机 on_peer：收到从机回执，finishByMode(peer=0x%04X, my=lastSend=0x%04X)\n",
               peer & 0xFFFF, s_netDriver.lastSend & 0xFFFF);
        fflush(stdout);
        finishByMode(sio, mode, peer, s_netDriver.lastSend);
    } else {
        unsigned my = readLocalSend(sio, mode);
        printf("[sio] MULTI 从机 on_peer：收到主机请求，回发 my=0x%04X 并 finishByMode(peer=0x%04X)\n",
               my & 0xFFFF, peer & 0xFFFF);
        fflush(stdout);
        mgba_net_send(mode, sio->siocnt, my & 0xFFFF, (my >> 16) & 0xFFFF, 1); /* source=1: 接收方回发 */
        finishByMode(sio, mode, peer, my);
    }
}

EMSCRIPTEN_KEEPALIVE void mgba_sio_detach(struct mCore* core) {
    if (!core) return;
    core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, NULL);
    s_netDriver.connected = 0;
    s_netDriver.pending = 0;
}

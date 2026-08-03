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
#include <mgba/internal/gba/gba.h>    /* GBARaiseIRQ, GBA_IRQ_SIO */

/* 前端注入的发送钩子。data_lo/data_hi 携带本机待发数据（按 mode 解析）。
 * source: 0=netStart（本机主动 start 发送），1=on_peer 接收方回发（收到对端请求后回发本机数据）。 */
EM_JS(void, mgba_net_send, (int mode, int siocnt, unsigned data_lo, unsigned data_hi, int source), {
    if (typeof Module.__netSioSend === 'function') {
        Module.__netSioSend(mode, siocnt, data_lo, data_hi, source);
    }
});

/* 帧锁步出向钩子（JS 转发给对端）。
 * mgba_net_frame：帧包 {帧号 n, 本帧 SIOMLT_SEND, 本帧是否 start 过}。
 * mgba_net_mode：本端 SIO mode 变化（JS 据此向对端发 mb/me 协商锁步激活）。 */
EM_JS(void, mgba_net_frame, (int n, unsigned send, int start), {
    if (typeof Module.__netFrameSend === 'function') {
        Module.__netFrameSend(n, send, start);
    }
});
EM_JS(void, mgba_net_mode, (int mode), {
    if (typeof Module.__netModeChange === 'function') {
        Module.__netModeChange(mode);
    }
});

/* 帧包环缓冲容量。S 最大 10，stall 超过 64 帧时靠 peerN 序号校验防误配。 */
#define LS_RING 64

struct GBASIONetDriver {
    struct GBASIODriver d;
    int connected;       /* 对端是否已连接 */
    int playerId;        /* MULTI 本机 id（0=主机,1=从机） */
    int pending;         /* 本机已发起 start（NORMAL8/32/MULTI），等待对端响应才完成 */
    int sending;         /* 重入保护：1 表示正在 mgba_net_send 中，防止帧同步触发递归 */
    unsigned lastSend;   /* MULTI 主机 finish 时回填 data[0] */

    /* ── 帧边界锁步（MULTI 专用，lockstepS>0 时启用；协议规则见 PLAN §6.3）── */
    int lockstepS;           /* 帧差容忍（0=关闭锁步，走旧异步模型） */
    int remoteMulti;         /* 对端已报进入 MULTI（mgba_sio_on_peer_mode） */
    int lsActive;            /* 锁步激活中：双端都在 MULTI，帧号对齐计数 */
    int lsFrame;             /* 激活以来已完成帧数（下一待跑帧号） */
    int lsStartedThisFrame;  /* 本帧内 netStart 已触发过（每帧最多一笔） */
    unsigned lsStartSend;    /* 本帧 start 时刻的 SIOMLT_SEND（帧包 send 字段） */
    unsigned lsLocalSend[LS_RING];   /* 本端各帧 send */
    int      lsLocalStart[LS_RING];  /* 本端各帧 start 标志 */
    unsigned lsPeerSend[LS_RING];    /* 对端各帧 send */
    int      lsPeerStart[LS_RING];   /* 对端各帧 start 标志 */
    int      lsPeerN[LS_RING];       /* 对端包帧号（校验防环缓冲回绕误配） */
    int      lsPeerValid[LS_RING];   /* 对端包是否已收到 */
};

static struct GBASIONetDriver s_netDriver;

static unsigned readLocalSend(struct GBASIO* sio, int mode) {
    switch (mode) {
    case GBA_SIO_NORMAL_8:
        return sio->p->memory.io[GBA_REG(SIODATA8)] & 0xFF;
    case GBA_SIO_NORMAL_32: {
        /* 诊断：打印实际从 memory.io 读到的 LO/HI，去重避免逐帧刷屏。
         * 用于定位"游戏写 SIODATA32_HI=0x0000 但 send hi=0xFFFF"的矛盾——
         * 若此处读到 0x0000 而 sio send 仍 0xFFFF，说明传参错；
         * 若此处读到 0xFFFF，说明 memory.io 在 readLocalSend 时被 finishByMode 覆盖或游戏写未生效。 */
        unsigned lo = sio->p->memory.io[GBA_REG(SIODATA32_LO)] & 0xFFFF;
        unsigned hi = sio->p->memory.io[GBA_REG(SIODATA32_HI)] & 0xFFFF;
        static unsigned s_lo = 0xFFFF, s_hi = 0xFFFF;
        if (lo != s_lo || hi != s_hi) {
            printf("[sio] readLocalSend N32 LO=0x%04X HI=0x%04X\n", lo, hi);
            fflush(stdout);
            s_lo = lo; s_hi = hi;
        }
        return lo | (hi << 16);
    }
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
        /* 强制 raise SIO IRQ：mGBA 的 FinishTransfer 仅在 SIOCNT 的 Irq 使能位(bit14)
         * 被设时才 raise IRQ。实测绿宝石 guest 端 SIOCNT 未设该位(0x200c)，
         * 导致 guest 收到 host 数据后无 SIO 中断，游戏逻辑不推进、不写自己的
         * SIODATA32_HI，一直回发 hi=0xFFFF，握手死锁。强制 raise 让从机也能
         * 收到传输完成通知。主机端已设 Irq 位会再 raise 一次，幂等无害。 */
        GBARaiseIRQ(sio->p, GBA_IRQ_SIO, 0);
        break;
    case GBA_SIO_NORMAL_32:
        GBASIONormal32FinishTransfer(sio, peer, 0);
        GBARaiseIRQ(sio->p, GBA_IRQ_SIO, 0);
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
        GBARaiseIRQ(sio->p, GBA_IRQ_SIO, 0);
        break;
    }
    default:
        break;
    }
}

/* SIO mode 枚举名（与 sio.h GBASIOMode 一致：0=NORMAL8 1=NORMAL32 2=MULTI 3=UART 8=GPIO 12=JOYBUS），便于 printf 可读。 */
static const char* sioModeStr(int m) {
    switch (m) {
        case 0: return "NORMAL8";
        case 1: return "NORMAL32";
        case 2: return "MULTI";
        case 3: return "UART";
        case 8: return "GPIO";
        case 12: return "JOYBUS";
        default: return "?";
    }
}

/* ── 帧边界锁步辅助（协议规则唯一事实来源，harness test-netplay.js 据此断言）── */

/* 完成一次 MULTI 传输：data[0]=主机 send、data[1]=从机 send（SIOMULTI 寄存器按
 * 玩家 id 编址，两台机器内容一致，与本地角色无关），2P 其余填 0xFFFF。 */
static void lsFinishMulti(struct GBASIO* sio, unsigned hostSend, unsigned guestSend) {
    uint16_t data[4];
    data[0] = (uint16_t)(hostSend & 0xFFFF);
    data[1] = (uint16_t)(guestSend & 0xFFFF);
    data[2] = 0xFFFF;
    data[3] = 0xFFFF;
    GBASIOMultiplayerFinishTransfer(sio, data, 0);
    /* 强制 raise IRQ：游戏可能未设 SIOCNT Irq 使能位，无 IRQ 则游戏逻辑不推进。 */
    GBARaiseIRQ(sio->p, GBA_IRQ_SIO, 0);
    printf("[sio] ls finish_multi host=0x%04X guest=0x%04X player=%d\n",
           hostSend & 0xFFFF, guestSend & 0xFFFF, s_netDriver.playerId);
    fflush(stdout);
}

/* 清锁步运行时状态（保留 lockstepS/connected/playerId 等 JS 配置）。 */
static void lsResetRuntime(struct GBASIONetDriver* n) {
    n->lsActive = 0;
    n->lsFrame = 0;
    n->lsStartedThisFrame = 0;
    n->lsStartSend = 0;
    memset(n->lsPeerValid, 0, sizeof(n->lsPeerValid));
    memset(n->lsLocalStart, 0, sizeof(n->lsLocalStart));
}

/* 激活判定：lockstepEnabled && 已连接 && 本端 MULTI && 对端已报 MULTI。
 * 激活时帧号归 0；退出时直接清 siocnt 的 Start/Busy 位（bit7）——不调 FinishTransfer，
 * 避免模式切换期按错误模式写别名寄存器（SIOMULTI0/1 == SIODATA32_LO/HI）。 */
static void lsUpdateActive(struct GBASIONetDriver* n, struct GBASIO* sio) {
    int was = n->lsActive;
    n->lsActive = n->lockstepS > 0 && n->connected && sio &&
                  sio->mode == GBA_SIO_MULTI && n->remoteMulti;
    if (n->lsActive && !was) {
        n->lsFrame = 0;
        n->lsStartedThisFrame = 0;
        memset(n->lsPeerValid, 0, sizeof(n->lsPeerValid));
        memset(n->lsLocalStart, 0, sizeof(n->lsLocalStart));
        printf("[sio] ls ACTIVATE S=%d player=%d（帧号归 0，开始门控）\n", n->lockstepS, n->playerId);
        fflush(stdout);
    } else if (!n->lsActive && was) {
        if (sio) sio->siocnt &= ~0x0080; /* 清 Start/Busy 位防卡死 */
        n->lsStartedThisFrame = 0;
        printf("[sio] ls DEACTIVATE（退出锁步，自由运行）\n");
        fflush(stdout);
    }
}

static bool netInit(struct GBASIODriver* d) { (void)d; return true; }
static void netDeinit(struct GBASIODriver* d) { (void)d; }
static void netReset(struct GBASIODriver* d) {
    struct GBASIONetDriver* n = (struct GBASIONetDriver*)d;
    n->pending = 0;
    n->lastSend = 0;
    n->sending = 0;
    /* 游戏复位（软重启）：清锁步运行时，保留 JS 配置；若仍在 MULTI 会经
     * lsUpdateActive 重新激活并帧号归 0，两端若都复位则自然重新对齐。 */
    lsResetRuntime(n);
    lsUpdateActive(n, d->p);
}
static uint32_t netId(const struct GBASIODriver* d) { (void)d; return 0x574F524E; /* 'NETW' */ }
static bool netLoadState(struct GBASIODriver* d, const void* s, size_t sz) { (void)d; (void)s; (void)sz; return false; }
static void netSaveState(struct GBASIODriver* d, void** s, size_t* sz) { (void)d; if (s) *s = NULL; if (sz) *sz = 0; }
static void netSetMode(struct GBASIODriver* d, enum GBASIOMode mode) {
    struct GBASIONetDriver* n = (struct GBASIONetDriver*)d;
    /* 模式切换时前一模式的待完成传输作废：清 pending/lastSend。否则残留 pending
     * 会阻塞新模式的 MULTI 兜底触发（netWriteSIOCNT 里 !pending 才触发），或让
     * 新模式 on_peer(src=1) 被误判为"发起方完成"。实测：宝可梦 RFU 探测失败后
     * 从 NORMAL32(pending=1) 切 MULTI，不清则 MULTI 兜底被卡。 */
    n->pending = 0;
    n->lastSend = 0;
    n->sending = 0;
    printf("[sio] setMode mode=%d(%s) pending cleared\n", (int)mode, sioModeStr((int)mode));
    fflush(stdout);
    /* 帧锁步：模式变化影响激活态（进出 MULTI），并通知前端发 mb/me 协商。 */
    lsUpdateActive(n, d->p);
    mgba_net_mode((int)mode);
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
static bool netStart(struct GBASIODriver* d); /* 前向声明：netWriteSIOCNT MULTI 兜底触发需调 */
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
        /* 兜底触发 MULTI 传输：绿宝石 MULTI mode 的 host 不写 SIOCNT Busy bit
         *（mGBA sio.c:193 要求 Busy 才 _startTransfer），导致无 MULTI 传输、双方死锁
         *（游戏轮询 SIOMULTI 等对端数据，driver 等游戏写 Busy）。driver 在 host 写
         * SIOCNT 且空闲时主动触发一次 netStart（模仿 host 写 Busy）。
         * 旧异步模型用 pending 防重复；帧锁步模型用 lsStartedThisFrame 限每帧一笔。 */
        if (s_netDriver.playerId == 0) {
            if (s_netDriver.lockstepS > 0) {
                if (!s_netDriver.lsStartedThisFrame) netStart(d);
            } else if (!s_netDriver.pending) {
                netStart(d);
            }
        }
    } else if (sio->mode == GBA_SIO_NORMAL_8 || sio->mode == GBA_SIO_NORMAL_32) {
        v = GBASIONormalFillSi(v);
    }
    return v;
}
static uint16_t netWriteRCNT(struct GBASIODriver* d, uint16_t v) {
    struct GBASIO* sio = d->p;
    /* 诊断：游戏写 RCNT 时记录（去重）。此前 netWriteRCNT 是 no-op 无日志，
     * 导致"游戏若只写 RCNT（不改 mode）则完全不可见"的诊断盲点——光明之魂2 实测
     * 日志无 setMode 无 writeSIOCNT，须靠此确认游戏到底有没有动 SIO。 */
    static uint16_t s_lastRcnt = 0xFFFF;
    if (v != s_lastRcnt) {
        printf("[sio] writeRCNT v=0x%04X mode=%d(%s) connected=%d\n",
               (unsigned)v & 0xFFFF, (int)sio->mode, sioModeStr((int)sio->mode), s_netDriver.connected);
        fflush(stdout);
        s_lastRcnt = v;
    }
    return v;
}

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
    /* 帧锁步（MULTI）：不网络发送、不置 pending。只记录"本帧 start 过 + start 时刻的
     * send"，return false 异步；完成推迟到 mgba_sio_frame_end 的帧边界（S 帧后，
     * 两端用同一份帧包数据各自本地 finish）。每帧最多记录一笔（协议上游戏每帧一笔）。 */
    if (n->lockstepS > 0 && mode == GBA_SIO_MULTI) {
        if (!n->lsStartedThisFrame) {
            n->lsStartedThisFrame = 1;
            n->lsStartSend = send;
            printf("[sio] ls netStart frame=%d player=%d send=0x%04X（记录，帧边界完成）\n",
                   n->lsFrame, n->playerId, send & 0xFFFF);
            fflush(stdout);
        }
        return false;
    }
    /* 诊断：mode: 0=NORMAL8 1=NORMAL32 2=MULTI 3=UART。playerId: 0=主机 1=从机。
     * 若 netStart 从不触发，说明游戏未进入 SIO 传输（mode/Ready/Si 位未就绪）。 */
    printf("[sio] netStart mode=%d player=%d pending=%d connected=%d siocnt=0x%04X send=0x%08X\n",
           mode, n->playerId, n->pending, n->connected, (unsigned)sio->siocnt & 0xFFFF, (unsigned)send);
    fflush(stdout);
    /* 先置 pending/lastSend 再 send：发送钩子可能同步递归回 on_peer（同步 harness 或
     * 极快回声），若 pending 未先置，回声到达时被判为"迟到响应"丢弃，发起方永不完成、
     * pending 反悬空。先置位保证任何时序下 on_peer 都能正确识别发起方态。 */
    n->pending = 1;
    if (mode == GBA_SIO_MULTI) {
        n->lastSend = send;
    }
    /* 置 sending 防重入：mgba_net_send 可能同步回调 on_peer（同步 harness），
     * 后者若触发帧同步 netStart，会在 sending 检查处跳过，防止无限递归。 */
    n->sending = 1;
    mgba_net_send(mode, sio->siocnt, send & 0xFFFF, (send >> 16) & 0xFFFF, 0); /* source=0: 本机查询 */
    n->sending = 0;
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
    s_netDriver.pending = 0; s_netDriver.lastSend = 0; s_netDriver.sending = 0;
    /* 帧锁步：配置清零（set_lockstep/set_peer/on_peer_mode 由 JS 重新下发）。 */
    s_netDriver.lockstepS = 0; s_netDriver.remoteMulti = 0;
    lsResetRuntime(&s_netDriver);
    core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, d);
    printf("[sio] attach ok (driver 已注册到 LINK_PORT)\n");
    fflush(stdout);
    return 1;
}

EMSCRIPTEN_KEEPALIVE void mgba_sio_set_peer(struct mCore* core, int connected, int playerId) {
    s_netDriver.connected = connected ? 1 : 0;
    s_netDriver.playerId = playerId;
    struct GBASIO* sio = (core && core->board) ? &((struct GBA*)core->board)->sio : NULL;
    /* 帧锁步：连接状态变化影响激活态。 */
    lsUpdateActive(&s_netDriver, sio);
    if (sio) {
        printf("[sio] set_peer connected=%d playerId=%d mode=%d(%s) siocnt=0x%04X rcnt=0x%04X\n",
               s_netDriver.connected, s_netDriver.playerId, (int)sio->mode, sioModeStr((int)sio->mode),
               (unsigned)sio->siocnt & 0xFFFF, (unsigned)sio->rcnt & 0xFFFF);
    } else {
        printf("[sio] set_peer connected=%d playerId=%d (no sio)\n", s_netDriver.connected, s_netDriver.playerId);
    }
    fflush(stdout);
    /* attach 后若游戏已在 MULTI/NORMAL mode，主动设就绪位（兜底：游戏可能不再 writeSIOCNT）。
     * 与 netWriteSIOCNT 同逻辑。NORMAL 设 Si 位让游戏判定对端在线、从机才会写握手数据。 */
    if (connected && sio) {
        if (sio->mode == GBA_SIO_MULTI) {
            /* 补正 SIOCNT 位：游戏写 SIOCNT=0x600F 时驱动未 attach，id=0 connected=0
             * 导致 GBASIOWriteSIOCNT 的 MULTI handler 设 Slave=1（sio.c:180），
             * 主机游戏读到 Slave=1 以为自己是从机，等主机发起 → 死锁。
             * 同理 Id 位 = 0 让双方都以为自己是主机。 */
            sio->siocnt = GBASIOMultiplayerSetSlave(sio->siocnt, !!s_netDriver.playerId);
            sio->siocnt = GBASIOMultiplayerSetId(sio->siocnt, s_netDriver.playerId);
            sio->siocnt = GBASIOMultiplayerSetReady(sio->siocnt, 1);
            /* 补设 RCNT 位：游戏写 SIOCNT=0x600F 时 mode 尚为 GPIO，GBASIOWriteSIOCNT
             * 的 MULTI handler 跳过了 SC=1 和 SD=1 的设置（sio.c:191）；后续游戏写
             * RCNT=0x0000 时 _switchMode 才切到 MULTI，但此时 sio->rcnt 已被清零。
             * 缩略语：SC=Serial Clock（主机驱动时钟），SD=Serial Data（数据线），
             * SI=Serial Input（对端 SO 的镜像）。设齐后游戏收到"电缆已激活"信号。 */
            sio->rcnt = GBASIORegisterRCNTFillSc(sio->rcnt);                /* SC=1 */
            sio->rcnt = GBASIORegisterRCNTSetSd(sio->rcnt, 1);              /* SD=1 */
            sio->rcnt = GBASIORegisterRCNTSetSi(sio->rcnt, !!s_netDriver.playerId); /* SI=1 对从机 */
            /* 兜底触发 MULTI 传输：游戏已在 MULTI mode 且已写 SIOCNT 后才 attach，
             * netWriteSIOCNT 不会被调用，兜底逻辑不会触发。此处补触发。
             * 主机（playerId=0）主动发起第一次传输以启动通信循环。 */
            if (s_netDriver.playerId == 0 && !s_netDriver.pending) {
                printf("[sio]   set_peer 兜底触发 MULTI netStart（游戏 attach 前已就绪）\n");
                fflush(stdout);
                netStart(&s_netDriver.d);
            }
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
    printf("[sio] on_peer mode=%d source=%d player=%d pending=%d peer=0x%04X\n",
           mode, source, s_netDriver.playerId, s_netDriver.pending, peer & 0xFFFF);
    fflush(stdout);

    /* bug3: 只处理与本机当前 mode 一致的帧。SIO 寄存器是别名的——
     * SIODATA32_LO/HI(0x120/0x122) == SIOMULTI0/1，SIODATA8(0x12A) == SIOMLT_SEND。
     * 模式切换期在途的异模式帧若照常 finish，会按错误模式写错寄存器。两端 mode 一致才处理。 */
    if (mode != (int)sio->mode) {
        printf("[sio] on_peer DROPPED: mode mismatch peer=%d(%s) local=%d(%s)\n",
               mode, sioModeStr(mode), (int)sio->mode, sioModeStr((int)sio->mode));
        fflush(stdout);
        return;
    }

    if (mode == GBA_SIO_NORMAL_8 || mode == GBA_SIO_NORMAL_32) {
        /* 双向异步 + source 防死循环。每条路径都明确：是否 finish、是否回发、是否清 pending。
         *   src=0 + !pending（接收方）：读 my → finish 写对端数据 + IRQ → 回发 my(src=1)。
         *   src=0 + pending（全双工）：用对端查询数据完成，清 pending，不回发（beacon 型握手）。
         *   src=1 + pending（发起方）：finish 写对端响应 + IRQ，清 pending，不回发。
         *   src=1 + !pending（迟到/重复响应）：丢弃，不 finish 不回发（bug1：否则覆写 SIODATA+假IRQ）。
         * source=1 永不回发 → 重复/迟到响应不会触发再回发，杜绝死循环。 */
        if (source == 0) {
            if (s_netDriver.pending) {
                s_netDriver.pending = 0;
                printf("[sio]   全双工：用对端查询完成本机传输（不回发）\n");
                finishByMode(sio, mode, peer, 0);
            } else {
                unsigned my = readLocalSend(sio, mode); /* finish 会覆盖 SIODATA，须先读 */
                printf("[sio]   接收方：finishByMode(peer=0x%04X) + 回发 my=0x%04X (src=1)\n",
                       peer & 0xFFFF, my & 0xFFFF);
                finishByMode(sio, mode, peer, 0);
                mgba_net_send(mode, sio->siocnt, my & 0xFFFF, (my >> 16) & 0xFFFF, 1);
            }
        } else {
            if (s_netDriver.pending) {
                s_netDriver.pending = 0;
                printf("[sio]   发起方：收到响应，finishByMode(peer=0x%04X) + IRQ（清 pending，不回发）\n",
                       peer & 0xFFFF);
                finishByMode(sio, mode, peer, 0);
            } else {
                printf("[sio]   迟到响应 DROPPED：本机未 pending，不 finish 不回发\n");
            }
        }
        return;
    }

    /* MULTI：只主机 start（sio.c:194）。source + pending 四象限明确：
     *   src=0 + !pending（从机收到主机查询）：读 my → finish 写对端 + IRQ → 回发 my(src=1)。
     *   src=1 + pending（主机收到从机回执）：finish(peer, lastSend) + IRQ，清 pending。
     *   src=0 + pending / src=1 + !pending：迟到/错位 → 丢弃，不 finish 不回发（bug2：否则两端互发 src=1
     *   无限回声风暴直至栈溢出——harness test-netplay.js 场景4 秒级复现）。 */
    if (mode == GBA_SIO_MULTI) {
        if (source == 0 && !s_netDriver.pending) {
            unsigned my = readLocalSend(sio, mode);
            printf("[sio] MULTI 从机 on_peer：收到主机查询，回发 my=0x%04X(src=1) + finishByMode(peer=0x%04X)\n",
                   my & 0xFFFF, peer & 0xFFFF);
            fflush(stdout);
            finishByMode(sio, mode, peer, my);
            mgba_net_send(mode, sio->siocnt, my & 0xFFFF, (my >> 16) & 0xFFFF, 1);
        } else if (source == 1 && s_netDriver.pending) {
            s_netDriver.pending = 0;
            printf("[sio] MULTI 主机 on_peer：收到从机回执，finishByMode(peer=0x%04X, my=lastSend=0x%04X)\n",
                   peer & 0xFFFF, s_netDriver.lastSend & 0xFFFF);
            fflush(stdout);
            finishByMode(sio, mode, peer, s_netDriver.lastSend);
            /* 帧同步触发：游戏不写 SIOCNT Busy 位，netWriteSIOCNT 的兜底不会触发。
             * 主机在每次收到从机回执后立即发起下一次传输，形成连续帧同步循环。
             * 游戏每帧收到 SIOMULTI 更新 + IRQ，才有机会写 SIOMLT_SEND 推进协议。
             * 加 sending 防重入（同步 harness 中 mgba_net_send 可能同步回调 on_peer）。 */
            if (s_netDriver.playerId == 0 && !s_netDriver.pending && !s_netDriver.sending) {
                printf("[sio]   MULTI 帧同步：触发下一次 netStart\n");
                fflush(stdout);
                netStart(&s_netDriver.d);
            }
        } else {
            printf("[sio] MULTI on_peer DROPPED: source=%d pending=%d（迟到/错位，不处理防风暴）\n",
                   source, s_netDriver.pending);
            fflush(stdout);
        }
        return;
    }
}

/* ==================== 帧边界锁步导出（MULTI 专用）====================
 * 协议规则（PLAN §6.3）：
 *   active = lockstepS>0 && connected && 本端 MULTI && 对端已报 MULTI；激活时帧号归 0。
 *   门控：跑第 m 帧前需对端包 m-S（m<S 不门控）→ mgba_sio_gate。
 *   帧包：frame_end(f) 记录本帧 {send,start} 并经 __netFrameSend 发出。
 *   完成：frame_end(f) 且 f>=S 时按帧 f-S 的 start 标志完成传输（主机看本端标志、
 *   从机看对端标志），数据取两端帧 f-S 的包 → 链路仿真延迟恒 S 帧，确定无错位。
 *   未激活：自由跑，start 的传输以 0xFFFF 兜底完成（模拟线缆空挂）。 */

EMSCRIPTEN_KEEPALIVE void mgba_sio_set_lockstep(struct mCore* core, int s) {
    if (!core) return;
    if (s < 0) s = 0;
    if (s > 10) s = 10; /* S 上限：再大链路仿真延迟不可玩，ring 也兜不住错位 */
    s_netDriver.lockstepS = s;
    struct GBASIO* sio = core->board ? &((struct GBA*)core->board)->sio : NULL;
    lsUpdateActive(&s_netDriver, sio);
    printf("[sio] set_lockstep S=%d\n", s_netDriver.lockstepS);
    fflush(stdout);
}

EMSCRIPTEN_KEEPALIVE void mgba_sio_on_peer_mode(struct mCore* core, int multi) {
    if (!core) return;
    s_netDriver.remoteMulti = multi ? 1 : 0;
    struct GBASIO* sio = core->board ? &((struct GBA*)core->board)->sio : NULL;
    lsUpdateActive(&s_netDriver, sio);
    printf("[sio] on_peer_mode multi=%d\n", s_netDriver.remoteMulti);
    fflush(stdout);
}

EMSCRIPTEN_KEEPALIVE int mgba_sio_gate(struct mCore* core) {
    if (!core || !core->board) return 1;
    if (!s_netDriver.lsActive) return 1;
    int k = s_netDriver.lsFrame - s_netDriver.lockstepS;
    if (k < 0) return 1;
    int slot = k % LS_RING;
    return (s_netDriver.lsPeerValid[slot] && s_netDriver.lsPeerN[slot] == k) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void mgba_sio_on_peer_frame(struct mCore* core, int n, unsigned send, int start) {
    if (!core || n < 0) return;
    int slot = n % LS_RING;
    s_netDriver.lsPeerSend[slot] = send & 0xFFFF;
    s_netDriver.lsPeerStart[slot] = start ? 1 : 0;
    s_netDriver.lsPeerN[slot] = n;
    s_netDriver.lsPeerValid[slot] = 1;
    /* 去重打印：每 60 帧（约 1 秒）报一次水位，便于观察锁步是否活着。 */
    if (n % 60 == 0) {
        printf("[sio] ls peer_frame n=%d send=0x%04X start=%d（local frame=%d）\n",
               n, send & 0xFFFF, start ? 1 : 0, s_netDriver.lsFrame);
        fflush(stdout);
    }
}

EMSCRIPTEN_KEEPALIVE unsigned mgba_sio_frame_end(struct mCore* core) {
    if (!core || !core->board) return 0;
    struct GBA* gba = core->board;
    struct GBASIO* sio = &gba->sio;
    struct GBASIONetDriver* n = &s_netDriver;
    if (n->lockstepS <= 0 || sio->mode != GBA_SIO_MULTI) {
        n->lsStartedThisFrame = 0;
        return 0;
    }
    unsigned send = n->lsStartedThisFrame ? n->lsStartSend
                                          : readLocalSend(sio, GBA_SIO_MULTI);
    int start = n->lsStartedThisFrame;
    n->lsStartedThisFrame = 0;
    if (!n->lsActive) {
        /* 未激活（对端未进 MULTI）：传输以 0xFFFF 兜底完成（硬件上空挂线缆时
         * 主机读回自己的 send、其余槽位 0xFFFF），不计帧、不发帧包。 */
        if (start) {
            unsigned hostSend = n->playerId == 0 ? send : 0xFFFF;
            unsigned guestSend = n->playerId == 1 ? send : 0xFFFF;
            printf("[sio] ls 未激活：start=0x%04X 以 0xFFFF 兜底完成\n", send & 0xFFFF);
            lsFinishMulti(sio, hostSend, guestSend);
        }
        return send;
    }
    int f = n->lsFrame;
    int slot = f % LS_RING;
    n->lsLocalSend[slot] = send;
    n->lsLocalStart[slot] = start;
    mgba_net_frame(f, send, start);
    if (f >= n->lockstepS) {
        int k = f - n->lockstepS;
        int ks = k % LS_RING;
        int peerOk = n->lsPeerValid[ks] && n->lsPeerN[ks] == k;
        /* 主机侧传输由本端 start 标志驱动；从机侧由对端（主机）帧包的 start 标志驱动。
         * 两端用同一份数据在各自的帧 f 边界本地完成 → 双端 SIOMULTI 内容一致。 */
        int hostStart = n->playerId == 0 ? n->lsLocalStart[ks]
                                         : (peerOk ? n->lsPeerStart[ks] : 0);
        if (hostStart) {
            unsigned hostSend = n->playerId == 0 ? n->lsLocalSend[ks] : n->lsPeerSend[ks];
            unsigned guestSend = n->playerId == 0 ? (peerOk ? n->lsPeerSend[ks] : 0xFFFF)
                                                  : n->lsLocalSend[ks];
            printf("[sio] ls 帧边界完成 frame=%d k=%d host=0x%04X guest=0x%04X\n",
                   f, k, hostSend & 0xFFFF, guestSend & 0xFFFF);
            lsFinishMulti(sio, hostSend, guestSend);
        }
    }
    n->lsFrame++;
    return send;
}

EMSCRIPTEN_KEEPALIVE void mgba_sio_finish_multi(struct mCore* core, unsigned hostSend, unsigned guestSend) {
    if (!core || !core->board) return;
    lsFinishMulti(&((struct GBA*)core->board)->sio, hostSend, guestSend);
}

/* 纯测试钩子：在不加载/运行真实游戏的情况下，直接触发一次 netStart，
 * 用于 harness 验证发起方路径（pending 置位、source=0 发送、return false 异步）。
 * 仅 test-netplay.js 使用，正常运行不调用。不动模型逻辑。 */
EMSCRIPTEN_KEEPALIVE void mgba_sio_test_start(struct mCore* core) {
    if (!core || !core->board) return;
    struct GBA* gba = core->board;
    struct GBASIO* sio = &gba->sio;
    if ((struct GBASIODriver*)&s_netDriver != sio->driver) return;
    netStart(&s_netDriver.d);
}

/* 纯测试钩子：读取本机 SIODATA32（LO|HI），供 harness 断言接收方写入的数据。
 * NORMAL32 用；其他 mode 返回 0。仅 test-netplay.js 使用。 */
EMSCRIPTEN_KEEPALIVE unsigned mgba_sio_test_read_siodata32(struct mCore* core) {
    if (!core || !core->board) return 0;
    struct GBA* gba = core->board;
    struct GBASIO* sio = &gba->sio;
    unsigned lo = sio->p->memory.io[GBA_REG(SIODATA32_LO)] & 0xFFFF;
    unsigned hi = sio->p->memory.io[GBA_REG(SIODATA32_HI)] & 0xFFFF;
    return lo | (hi << 16);
}

/* 纯测试钩子：直接设置 sio->mode，让 harness 不依赖真实游戏写 SIOCNT 即可
 * 切到 NORMAL32/MULTI 测试对应分支。仅 test-netplay.js 使用，不动模型逻辑。
 * 与真实模式切换同路径：触发激活判定与模式通知（harness 可捕获 __netModeChange）。 */
EMSCRIPTEN_KEEPALIVE void mgba_sio_test_set_mode(struct mCore* core, int mode) {
    if (!core || !core->board) return;
    struct GBA* gba = core->board;
    struct GBASIO* sio = &gba->sio;
    sio->mode = (enum GBASIOMode)mode;
    lsUpdateActive(&s_netDriver, sio);
    mgba_net_mode(mode);
}

/* 纯测试钩子：写本机 SIOMLT_SEND，让 harness 模拟"游戏已 arm 发送寄存器"。 */
EMSCRIPTEN_KEEPALIVE void mgba_sio_test_write_send(struct mCore* core, unsigned v) {
    if (!core || !core->board) return;
    struct GBASIO* sio = &((struct GBA*)core->board)->sio;
    sio->p->memory.io[GBA_REG(SIOMLT_SEND)] = v & 0xFFFF;
}

/* 纯测试钩子：读 SIOMULTI0..3（idx 0-3），供 harness 断言 finish_multi 写入的数据。 */
EMSCRIPTEN_KEEPALIVE unsigned mgba_sio_test_read_multireg(struct mCore* core, int idx) {
    if (!core || !core->board || idx < 0 || idx > 3) return 0xFFFF;
    struct GBASIO* sio = &((struct GBA*)core->board)->sio;
    return sio->p->memory.io[GBA_REG(SIOMULTI0) + idx] & 0xFFFF;
}

EMSCRIPTEN_KEEPALIVE void mgba_sio_detach(struct mCore* core) {
    if (!core) return;
    core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, NULL);
    s_netDriver.connected = 0;
    s_netDriver.pending = 0;
    s_netDriver.sending = 0;
    /* 断开联机：锁步一并关闭（gate 恒放行，不再发帧包/模式通知之外的状态）。 */
    s_netDriver.lockstepS = 0;
    s_netDriver.remoteMulti = 0;
    lsResetRuntime(&s_netDriver);
}

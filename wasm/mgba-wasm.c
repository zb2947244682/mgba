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

/* 前端注入的发送钩子。data_lo/data_hi 携带本机待发数据（按 mode 解析）。 */
EM_JS(void, mgba_net_send, (int mode, int siocnt, unsigned data_lo, unsigned data_hi), {
    if (typeof Module.__netSioSend === 'function') {
        Module.__netSioSend(mode, siocnt, data_lo, data_hi);
    }
});

struct GBASIONetDriver {
    struct GBASIODriver d;
    int connected;       /* 对端是否已连接 */
    int playerId;        /* MULTI 本机 id（0=主机,1=从机） */
    int pending;         /* 本机已发起 start，等待对端回执 */
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
static void netSetMode(struct GBASIODriver* d, enum GBASIOMode mode) { (void)d; (void)mode; }
static bool netHandlesMode(struct GBASIODriver* d, enum GBASIOMode mode) {
    (void)d;
    return mode == GBA_SIO_NORMAL_8 || mode == GBA_SIO_NORMAL_32 || mode == GBA_SIO_MULTI;
}
static int netConnected(struct GBASIODriver* d) {
    return ((struct GBASIONetDriver*)d)->connected ? 1 : 0;
}
static int netDeviceId(struct GBASIODriver* d) {
    return ((struct GBASIONetDriver*)d)->playerId;
}
static uint16_t netWriteSIOCNT(struct GBASIODriver* d, uint16_t v) { (void)d; return v; }
static uint16_t netWriteRCNT(struct GBASIODriver* d, uint16_t v) { (void)d; return v; }

static bool netStart(struct GBASIODriver* d) {
    struct GBASIONetDriver* n = (struct GBASIONetDriver*)d;
    struct GBASIO* sio = d->p;
    int mode = (int)sio->mode;
    unsigned send = readLocalSend(sio, mode);
    n->pending = 1;
    n->lastSend = send;
    mgba_net_send(mode, sio->siocnt, send & 0xFFFF, (send >> 16) & 0xFFFF);
    return false; /* 异步完成，等对端回执由 mgba_sio_on_peer 调 FinishTransfer */
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
    /* 异步路径不经过 _sioFinish，finish* 回调不会被核心调用，置 NULL */
    d->finishMultiplayer = NULL; d->finishNormal8 = NULL; d->finishNormal32 = NULL;
    s_netDriver.connected = 0; s_netDriver.playerId = 0;
    s_netDriver.pending = 0; s_netDriver.lastSend = 0;
    core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, d);
    return 1;
}

EMSCRIPTEN_KEEPALIVE void mgba_sio_set_peer(struct mCore* core, int connected, int playerId) {
    (void)core;
    s_netDriver.connected = connected ? 1 : 0;
    s_netDriver.playerId = playerId;
}

EMSCRIPTEN_KEEPALIVE void mgba_sio_on_peer(struct mCore* core, int mode, unsigned data_lo, unsigned data_hi) {
    if (!core || !core->board) return;
    struct GBA* gba = core->board;
    struct GBASIO* sio = &gba->sio;
    if ((struct GBASIODriver*)&s_netDriver != sio->driver) return; /* 未 attach */
    unsigned peer = (data_lo & 0xFFFF) | ((data_hi & 0xFFFF) << 16);
    if (s_netDriver.pending) {
        /* 回执：我是发起方，完成传输 */
        s_netDriver.pending = 0;
        finishByMode(sio, mode, peer, s_netDriver.lastSend);
    } else {
        /* 请求：我是接收方，回发本机数据并完成 */
        unsigned my = readLocalSend(sio, mode);
        mgba_net_send(mode, sio->siocnt, my & 0xFFFF, (my >> 16) & 0xFFFF);
        finishByMode(sio, mode, peer, my);
    }
}

EMSCRIPTEN_KEEPALIVE void mgba_sio_detach(struct mCore* core) {
    if (!core) return;
    core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, NULL);
    s_netDriver.connected = 0;
    s_netDriver.pending = 0;
}

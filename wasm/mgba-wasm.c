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
    return GBACoreCreate();
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

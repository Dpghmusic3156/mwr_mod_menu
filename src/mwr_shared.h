#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

struct ModState {
    int32_t aimbot;        // 0x00
    float   aimFovPx;      // 0x04
    float   aimSmooth;     // 0x08
    int32_t drawFov;       // 0x0C
    int32_t aimTeamCheck;  // 0x10
    int32_t noRecoil;      // 0x14
    int32_t noSpread;      // 0x18
    int32_t rapidFire;     // 0x1C
    int32_t espEnabled;    // 0x20
    int32_t espBox;        // 0x24
    int32_t espLine;       // 0x28
    int32_t espHp;         // 0x2C
    int32_t espInfo;       // 0x30
    int32_t espTeamCheck;  // 0x34
    float   espMaxDist;    // 0x38
    int32_t speedHack;     // 0x3C
    float   speedMult;     // 0x40
    int32_t lowGravity;    // 0x44
    float   gravityScale;  // 0x48
    int32_t acBypass;      // 0x4C
    int32_t reqRescan;     // 0x50
    int32_t reqReset;      // 0x54
    int32_t reqKill;       // 0x58
    int32_t aimVisCheck;   // 0x5C
    int32_t aimPriority;   // 0x60 (0=Near FOV, 1=Closest, 2=Low HP)
    int32_t aimTracer;     // 0x64
};

struct ModTelemetry {
    uint64_t playerMgrAddr;    // 0x00
    uint64_t localHeroAddr;    // 0x08
    uint64_t localPlayerAddr;  // 0x10
    uint64_t camMgrAddr;       // 0x18
    uint64_t nativeCamAddr;    // 0x20
    int32_t  playerCount;      // 0x28
    int32_t  enemyCount;       // 0x2C
    float    localX;           // 0x30
    float    localY;           // 0x34
    float    localZ;           // 0x38
    float    localHp;          // 0x3C
    float    localMaxHp;       // 0x40
    int32_t  localTeamId;      // 0x44
    char     statusText[64];   // 0x48
};

static constexpr int kMaxEspEntities = 64;

struct EspEntity {
    uint64_t nativeTfPtr;   // 0x00
    uint64_t nativeRendPtr; // 0x08
    float    x;             // 0x10
    float    y;             // 0x14
    float    z;             // 0x18
    float    hp;            // 0x1C
    float    maxHp;         // 0x20
    int32_t  isEnemy;       // 0x24
    uint8_t  renderShow;    // 0x28
    uint8_t  _pad[3];       // 0x29
    char     name[20];      // 0x2C
}; // 0x40 = 64 bytes

struct EspSnapshot {
    uint32_t  seq;                       // 0x00 (odd = writer busy, even = stable)
    int32_t   count;                     // 0x04
    uint64_t  nativeCamPtr;              // 0x08
    uint64_t  playerCameraPtr;           // 0x10
    uint64_t  camTfPtr;                  // 0x18
    uint64_t  recoilSpringPtr;           // 0x20
    float     fallbackVp[16];            // 0x28 (64 bytes)
    float     camPos[3];                 // 0x68
    int32_t   _pad;                      // 0x74
    EspEntity entities[kMaxEspEntities]; // 0x78
};

struct MwrCamOffsets {
    uint32_t lookAngles;
    uint32_t recoilAngles;
    uint32_t recoilEnabled;
    uint32_t springPos;
    uint32_t springClearLen;
    uint32_t nativeCamVp;
};

// Shared internal helpers implemented in mwr_imgui.cpp
extern std::atomic<bool> g_fridaControlled;
extern MwrCamOffsets     g_mwrCamOff;
void MwrResetStateToDefaults();
bool MwrIsValidAddrRange(uint64_t addr);
bool MwrIsValidUserPtr(uint64_t p);
bool MwrIsPageReadable(uint64_t pageBase);
bool MwrSafeReadMem(uint64_t addr, void* dst, size_t len);

template <typename T>
inline bool MwrSafeRead(uint64_t addr, T& out) {
    return MwrSafeReadMem(addr, &out, sizeof(T));
}

extern "C" {
ModState*     imgui_get_state();
ModTelemetry* imgui_get_telemetry();
EspSnapshot*  imgui_get_esp_snapshot();
void          imgui_on_swap(void* dpy, void* surf);
int           imgui_on_touch(int action, int pointerId, float x, float y);
void          imgui_toggle_menu();
void          imgui_set_menu_open(int open);
int           imgui_is_menu_open();
void          imgui_reset_lifecycle();
void          imgui_request_shutdown();
int           imgui_is_shutdown_done();
void          imgui_get_surface_size(int* outW, int* outH);
}

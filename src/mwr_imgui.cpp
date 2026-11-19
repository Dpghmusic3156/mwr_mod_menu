#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "mwr_shared.h"

#define LOG_TAG "MwrImGui"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct TouchEvent {
    int   action;
    float x;
    float y;
};

struct RectF {
    float x0, y0, x1, y1;
};

std::atomic<bool> g_fridaControlled{false};
MwrCamOffsets     g_mwrCamOff{0x50, 0x58, 0x60, 0x40, 0x3C, 0xdc};

// ============================================================================
// Global State
// ============================================================================
static ModState g_state = {
    0,      // aimbot
    160.0f, // aimFovPx
    4.0f,   // aimSmooth
    1,      // drawFov
    1,      // aimTeamCheck
    0,      // noRecoil
    0,      // noSpread
    0,      // rapidFire
    0,      // espEnabled
    1,      // espBox
    1,      // espLine
    1,      // espHp
    1,      // espInfo
    1,      // espTeamCheck
    250.0f, // espMaxDist
    0,      // speedHack
    1.5f,   // speedMult
    0,      // lowGravity
    0.45f,  // gravityScale
    0,      // acBypass
    0,      // reqRescan
    0,      // reqReset
    0,      // reqKill
    0,      // aimVisCheck
    0,      // aimPriority
    1,      // aimTracer
};

static ModTelemetry g_tele = {};
static EspSnapshot  g_esp  = {};

static std::atomic<bool> g_initialized{false};
static std::atomic<bool> g_shutdownReq{false};
static std::atomic<bool> g_shutdownDone{false};
static std::atomic<bool> g_menuOpen{false};
static std::atomic<int>  g_surfaceW{1600};
static std::atomic<int>  g_surfaceH{900};

static std::mutex              g_touchMtx;
static std::vector<TouchEvent> g_touchQueue;
static std::mutex              g_rectMtx;
static std::vector<RectF>      g_activeRects;
static int                     g_menuPointerId = -1;
static bool                    g_needClearTouchPos = false;

static bool   g_pillDragging = false;
static ImVec2 g_pillPos = ImVec2(24.0f, 36.0f);
static bool   g_pillPosInit = false;

static std::chrono::steady_clock::time_point g_lastFrameTime;

// ============================================================================
// Per-Thread Page-Cached Safe Memory Reader
// ============================================================================
static std::mutex g_pipeMtx;
static int        g_safePipe[2] = {-1, -1};
static constexpr int kPageCacheSize = 64;
static thread_local uint64_t t_pageCache[kPageCacheSize] = {};
static thread_local int      t_pageCacheCount = 0;

static void InitSafePipeLocked() {
    if (g_safePipe[0] == -1) {
        if (pipe2(g_safePipe, O_NONBLOCK) != 0) {
            g_safePipe[0] = -1;
            g_safePipe[1] = -1;
        }
    }
}

static void CloseSafePipe() {
    std::lock_guard<std::mutex> lk(g_pipeMtx);
    if (g_safePipe[0] != -1) { close(g_safePipe[0]); g_safePipe[0] = -1; }
    if (g_safePipe[1] != -1) { close(g_safePipe[1]); g_safePipe[1] = -1; }
}

static inline void ResetFramePageCache() {
    t_pageCacheCount = 0;
}

bool MwrIsValidAddrRange(uint64_t p) {
    const uint64_t untagged = p & 0x00ffffffffffffffULL;
    return untagged >= 0x10000000ULL && untagged < 0x0000ffffffffffffULL;
}

bool MwrIsValidUserPtr(uint64_t p) {
    const uint64_t untagged = p & 0x00ffffffffffffffULL;
    return untagged >= 0x10000000ULL && untagged < 0x0000ffffffffffffULL && (untagged & 3ULL) == 0;
}

static inline bool IsValidUserPtr(uint64_t p) {
    return MwrIsValidUserPtr(p);
}

bool MwrIsPageReadable(uint64_t pageBase) {
    for (int i = 0; i < t_pageCacheCount; ++i) {
        if (t_pageCache[i] == pageBase) return true;
    }
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(g_pipeMtx);
        if (g_safePipe[1] == -1) InitSafePipeLocked();
        if (g_safePipe[1] == -1) return false;

        ssize_t w = write(g_safePipe[1], reinterpret_cast<const void*>(pageBase), 1);
        if (w == 1) {
            uint8_t dummy = 0;
            ok = (read(g_safePipe[0], &dummy, 1) == 1);
        }
    }
    if (!ok) return false;

    if (t_pageCacheCount < kPageCacheSize) {
        t_pageCache[t_pageCacheCount++] = pageBase;
    }
    return true;
}

bool MwrSafeReadMem(uint64_t addr, void* dst, size_t len) {
    if (!MwrIsValidAddrRange(addr) || len == 0 || len > 512) return false;
    const uint64_t firstPage = addr & ~4095ULL;
    const uint64_t lastPage  = (addr + len - 1ULL) & ~4095ULL;
    if (!MwrIsPageReadable(firstPage)) return false;
    if (lastPage != firstPage && !MwrIsPageReadable(lastPage)) return false;
    std::memcpy(dst, reinterpret_cast<const void*>(addr), len);
    return true;
}

static bool SafeReadMem(uint64_t addr, void* dst, size_t len) {
    return MwrSafeReadMem(addr, dst, len);
}

template <typename T>
static inline bool SafeRead(uint64_t addr, T& out) {
    return SafeReadMem(addr, &out, sizeof(T));
}

// ============================================================================
// Unity TransformAccess + Renderer Visibility + Camera VP WorldToScreen
// ============================================================================
static bool IsUnityRendererVisible(uint64_t natRend) {
    if (!IsValidUserPtr(natRend)) return true; // no renderer ref -> don't falsely drop
    uint8_t flag129 = 0;
    int32_t visIdx = -1;
    // Verified in libunity.so UnityEngine.Renderer::get_isVisible (0x6e40d0..0x6e40e8)
    if (!SafeReadMem(natRend + 0x129, &flag129, 1)) return true;
    if ((flag129 & 0x10) == 0) return false;
    if (!SafeRead(natRend + 0x168, visIdx)) return true;
    return visIdx != -1;
}

static bool ReadUnityTransformPos(uint64_t nativeTf, float& ox, float& oy, float& oz) {
    uint64_t tfAccess = 0;
    int32_t idx = -1;
    if (!SafeRead(nativeTf + 0x38, tfAccess) || !SafeRead(nativeTf + 0x40, idx)) return false;
    if (!IsValidUserPtr(tfAccess) || idx < 0 || idx > 65536) return false;

    uint64_t pMatrices = 0, pIndices = 0;
    if (!SafeRead(tfAccess + 0x18, pMatrices) || !SafeRead(tfAccess + 0x20, pIndices)) return false;
    if (!IsValidUserPtr(pMatrices) || !IsValidUserPtr(pIndices)) return false;

    struct TRS {
        float px, py, pz, pw;
        float qx, qy, qz, qw;
        float sx, sy, sz, sw;
    };

    TRS trs{};
    if (!SafeRead(pMatrices + static_cast<uint64_t>(idx) * 48ULL, trs)) return false;
    float rx = trs.px, ry = trs.py, rz = trs.pz;

    int32_t cur = -1;
    if (!SafeRead(pIndices + static_cast<uint64_t>(idx) * 4ULL, cur)) return false;

    int depth = 0;
    while (cur >= 0 && cur < 65536 && depth < 48) {
        if (!SafeRead(pMatrices + static_cast<uint64_t>(cur) * 48ULL, trs)) return false;
        const float vx = rx * trs.sx;
        const float vy = ry * trs.sy;
        const float vz = rz * trs.sz;

        const float ix =  trs.qw * vx + trs.qy * vz - trs.qz * vy;
        const float iy =  trs.qw * vy + trs.qz * vx - trs.qx * vz;
        const float iz =  trs.qw * vz + trs.qx * vy - trs.qy * vx;
        const float iw = -trs.qx * vx - trs.qy * vy - trs.qz * vz;

        rx = ix * trs.qw + iw * -trs.qx + iy * -trs.qz - iz * -trs.qy + trs.px;
        ry = iy * trs.qw + iw * -trs.qy + iz * -trs.qx - ix * -trs.qz + trs.py;
        rz = iz * trs.qw + iw * -trs.qz + ix * -trs.qy - iy * -trs.qx + trs.pz;

        if (!SafeRead(pIndices + static_cast<uint64_t>(cur) * 4ULL, cur)) break;
        ++depth;
    }
    if (!std::isfinite(rx) || !std::isfinite(ry) || !std::isfinite(rz)) return false;
    ox = rx; oy = ry; oz = rz;
    return true;
}

// Column-major 4x4 VP matrix (Proj * View) at Unity Camera +0xdc
static inline bool WorldToScreen(const float m[16], float wx, float wy, float wz,
                                 int sw, int sh, float& sx, float& sy) {
    const float clipX = m[0] * wx + m[4] * wy + m[8]  * wz + m[12];
    const float clipY = m[1] * wx + m[5] * wy + m[9]  * wz + m[13];
    const float clipW = m[3] * wx + m[7] * wy + m[11] * wz + m[15];
    if (clipW < 0.05f || !std::isfinite(clipW)) return false;
    const float invW = 1.0f / clipW;
    sx = (static_cast<float>(sw) * 0.5f) * (1.0f + clipX * invW);
    sy = (static_cast<float>(sh) * 0.5f) * (1.0f - clipY * invW);
    return std::isfinite(sx) && std::isfinite(sy);
}

void MwrResetStateToDefaults() {
    g_state.aimbot       = 0;
    g_state.aimFovPx     = 160.0f;
    g_state.aimSmooth    = 4.0f;
    g_state.drawFov      = 1;
    g_state.aimTeamCheck = 1;
    g_state.aimVisCheck  = 0;
    g_state.aimPriority  = 0;
    g_state.aimTracer    = 1;
    g_state.noRecoil     = 0;
    g_state.noSpread     = 0;
    g_state.rapidFire    = 0;
    g_state.espEnabled   = 0;
    g_state.espBox       = 1;
    g_state.espLine      = 1;
    g_state.espHp        = 1;
    g_state.espInfo      = 1;
    g_state.espTeamCheck = 1;
    g_state.espMaxDist   = 250.0f;
    g_state.speedHack    = 0;
    g_state.speedMult    = 1.5f;
    g_state.lowGravity   = 0;
    g_state.gravityScale = 0.45f;
    g_state.acBypass     = 0;
    g_state.reqReset     = 1;
}

static inline void ResetStateToDefaults() {
    MwrResetStateToDefaults();
}

#include "mwr_ui.h"

// ============================================================================
// Background Overlay Renderer + 120Hz Native Aimbot + 120Hz NoRecoil
// ============================================================================
static void DrawEspAndAimbot(int sw, int sh) {
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    const float scale = std::clamp(std::min(sw, sh) / 450.0f, 1.2f, 2.2f);
    const bool needAim = (g_state.aimbot != 0);
    const bool needEsp = (g_state.espEnabled != 0);
    const bool needNoRec = (g_state.noRecoil != 0);

    if (needAim && g_state.drawFov != 0) {
        bg->AddCircle(ImVec2(sw * 0.5f, sh * 0.5f), g_state.aimFovPx,
                      IM_COL32(99, 176, 250, 170), 64, 1.5f);
    }

    if (!needEsp && !needAim && !needNoRec) return;

    ResetFramePageCache();

    // Lock-free snapshot copy
    EspSnapshot snap{};
    for (int retry = 0; retry < 3; ++retry) {
        uint32_t s1 = __atomic_load_n(&g_esp.seq, __ATOMIC_ACQUIRE);
        if (s1 & 1U) continue;
        std::memcpy(&snap, &g_esp, sizeof(EspSnapshot));
        uint32_t s2 = __atomic_load_n(&g_esp.seq, __ATOMIC_ACQUIRE);
        if (s1 == s2) break;
    }

    // 120Hz No Recoil enforcement (zeros m_RecoilAngles + RotationSpring_Recoil)
    if (needNoRec) {
        if (IsValidUserPtr(snap.playerCameraPtr)) {
            float recCheck[2]{};
            if (SafeReadMem(snap.playerCameraPtr + g_mwrCamOff.recoilAngles, recCheck, sizeof(recCheck))) {
                *reinterpret_cast<uint8_t*>(snap.playerCameraPtr + g_mwrCamOff.recoilEnabled) = 0;
                float* recPtr = reinterpret_cast<float*>(snap.playerCameraPtr + g_mwrCamOff.recoilAngles);
                recPtr[0] = 0.0f;
                recPtr[1] = 0.0f;
            }
        }
        if (IsValidUserPtr(snap.recoilSpringPtr)) {
            float vecZero[3]{};
            const uint32_t spOff = g_mwrCamOff.springPos;
            const uint32_t spLen = std::min<uint32_t>(g_mwrCamOff.springClearLen, 64U);
            if (SafeReadMem(snap.recoilSpringPtr + spOff, vecZero, sizeof(vecZero))) {
                std::memset(reinterpret_cast<void*>(snap.recoilSpringPtr + spOff), 0, spLen);
            }
        }
    }

    if (!needEsp && !needAim) return;
    if (snap.count <= 0 || snap.count > kMaxEspEntities) return;

    float vp[16]{};
    bool haveVp = false;
    if (IsValidUserPtr(snap.nativeCamPtr)) {
        haveVp = SafeReadMem(snap.nativeCamPtr + g_mwrCamOff.nativeCamVp, vp, sizeof(vp));
        if (haveVp && (std::fabs(vp[0]) < 1e-4f && std::fabs(vp[5]) < 1e-4f)) {
            // Probe nearby offsets in case a Unity Engine update shifted Camera::m_WorldToClipMatrix
            for (uint32_t probe = 0xc0; probe <= 0x130; probe += 4) {
                float cand[16]{};
                if (SafeReadMem(snap.nativeCamPtr + probe, cand, sizeof(cand)) &&
                    std::isfinite(cand[0]) && std::isfinite(cand[5]) && std::isfinite(cand[15]) &&
                    cand[0] > 0.2f && cand[0] < 10.0f && cand[5] > 0.2f && cand[5] < 10.0f &&
                    std::fabs(cand[3]) <= 1.5f && std::fabs(cand[7]) <= 1.5f &&
                    std::fabs(cand[11]) <= 1.5f && std::fabs(cand[15]) > 0.01f) {
                    std::memcpy(vp, cand, sizeof(vp));
                    g_mwrCamOff.nativeCamVp = probe;
                    break;
                }
            }
        }
    }
    if (!haveVp) {
        std::memcpy(vp, snap.fallbackVp, sizeof(vp));
    }
    if (std::fabs(vp[0]) < 1e-4f && std::fabs(vp[5]) < 1e-4f) return;

    float camX = snap.camPos[0], camY = snap.camPos[1], camZ = snap.camPos[2];
    if (IsValidUserPtr(snap.camTfPtr)) {
        float cx, cy, cz;
        if (ReadUnityTransformPos(snap.camTfPtr, cx, cy, cz)) {
            camX = cx; camY = cy; camZ = cz;
        }
    }

    const float scrCx = sw * 0.5f;
    const float scrCy = sh * 0.5f;
    const float fovR2 = g_state.aimFovPx * g_state.aimFovPx;
    const int   prio  = std::clamp(g_state.aimPriority, 0, 2);

    float bestScore = 1e30f;
    float bestTie   = 1e30f;
    bool  hasAimTarget = false;
    float aimX = 0.0f, aimY = 0.0f, aimZ = 0.0f;
    float aimScrX = 0.0f, aimScrY = 0.0f;
    int   aimTargetIdx = -1;

    for (int i = 0; i < snap.count; ++i) {
        const EspEntity& ent = snap.entities[i];
        if (ent.hp <= 0.0f && ent.maxHp > 0.0f) continue;

        float wx = ent.x, wy = ent.y, wz = ent.z;
        if (IsValidUserPtr(ent.nativeTfPtr)) {
            float tx, ty, tz;
            if (ReadUnityTransformPos(ent.nativeTfPtr, tx, ty, tz)) {
                wx = tx; wy = ty; wz = tz;
            }
        }

        const float dxC = wx - camX;
        const float dyC = wy - camY;
        const float dzC = wz - camZ;
        const float dist = std::sqrt(dxC * dxC + dyC * dyC + dzC * dzC);

        // Aimbot target selection
        if (needAim && (!g_state.aimTeamCheck || ent.isEnemy)) {
            float ax = 0, ay = 0;
            const float targetAimY = wy + 1.15f;
            if (WorldToScreen(vp, wx, targetAimY, wz, sw, sh, ax, ay)) {
                const float adx = ax - scrCx;
                const float ady = ay - scrCy;
                const float d2 = adx * adx + ady * ady;
                if (d2 <= fovR2) {
                    bool visibleOk = true;
                    if (g_state.aimVisCheck) {
                        if (ax < 0.0f || ax > static_cast<float>(sw) ||
                            ay < 0.0f || ay > static_cast<float>(sh) ||
                            ent.renderShow == 0 ||
                            !IsUnityRendererVisible(ent.nativeRendPtr)) {
                            visibleOk = false;
                        }
                    }
                    if (visibleOk) {
                        float score = d2;
                        if (prio == 1) score = dist;
                        else if (prio == 2) score = (ent.hp > 0.0f) ? ent.hp : 9999.0f;

                        if (score < bestScore || (std::fabs(score - bestScore) < 1e-3f && d2 < bestTie)) {
                            bestScore = score;
                            bestTie   = d2;
                            hasAimTarget = true;
                            aimX = wx; aimY = targetAimY; aimZ = wz;
                            aimScrX = ax; aimScrY = ay;
                            aimTargetIdx = i;
                        }
                    }
                }
            }
        }

        if (!needEsp) continue;
        if (g_state.espTeamCheck && !ent.isEnemy) continue;
        if (dist > g_state.espMaxDist) continue;

        float footX = 0, footY = 0, headX = 0, headY = 0;
        if (!WorldToScreen(vp, wx, wy, wz, sw, sh, footX, footY)) continue;
        if (!WorldToScreen(vp, wx, wy + 1.55f, wz, sw, sh, headX, headY)) continue;

        const float boxH = std::max(8.0f, footY - headY);
        const float boxW = boxH * 0.54f;
        const float x0 = headX - boxW * 0.5f;
        const float y0 = headY;
        const float x1 = headX + boxW * 0.5f;
        const float y1 = footY;

        if (x1 < -50.0f || x0 > sw + 50.0f || y1 < -50.0f || y0 > sh + 50.0f) continue;

        const bool isLocked = (i == aimTargetIdx);
        const ImU32 boxCol = isLocked
            ? IM_COL32(255, 214, 70, 255)
            : (ent.isEnemy ? IM_COL32(255, 92, 108, 235) : IM_COL32(99, 176, 250, 220));

        if (g_state.espLine) {
            bg->AddLine(ImVec2(scrCx, 0.0f), ImVec2(headX, y0),
                        isLocked ? IM_COL32(255, 214, 70, 210) : IM_COL32(255, 92, 108, 140),
                        isLocked ? 1.8f : 1.2f);
        }

        if (g_state.espBox) {
            bg->AddRect(ImVec2(x0 - 1.0f, y0 - 1.0f), ImVec2(x1 + 1.0f, y1 + 1.0f),
                        IM_COL32(0, 0, 0, 180), 0.0f, 0, 2.5f);
            bg->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), boxCol, 0.0f, 0, isLocked ? 2.2f : 1.5f);
        }

        if (g_state.espHp && ent.maxHp > 0.0f) {
            const float frac = std::clamp(ent.hp / ent.maxHp, 0.0f, 1.0f);
            const float barW = 4.0f;
            const float bx0 = x0 - barW - 4.0f;
            const float fillTop = y1 - boxH * frac;
            const ImU32 hpCol = IM_COL32(
                static_cast<int>(255 * (1.0f - frac)),
                static_cast<int>(220 * frac + 35),
                70, 240);
            bg->AddRectFilled(ImVec2(bx0 - 1.0f, y0 - 1.0f), ImVec2(bx0 + barW + 1.0f, y1 + 1.0f),
                              IM_COL32(0, 0, 0, 180));
            bg->AddRectFilled(ImVec2(bx0, fillTop), ImVec2(bx0 + barW, y1), hpCol);
        }

        if (g_state.espInfo) {
            char label[64];
            if (ent.name[0]) {
                std::snprintf(label, sizeof(label), "%s [%.0fm]", ent.name, dist);
            } else {
                std::snprintf(label, sizeof(label), "Player [%.0fm]", dist);
            }
            if (g_fontSmall) ImGui::PushFont(g_fontSmall);
            ImVec2 tSz = ImGui::CalcTextSize(label);
            const float tx = headX - tSz.x * 0.5f;
            const float ty = y0 - tSz.y - 4.0f * scale;
            bg->AddText(ImVec2(tx + 1.0f, ty + 1.0f), IM_COL32(0, 0, 0, 210), label);
            bg->AddText(ImVec2(tx, ty), isLocked ? IM_COL32(255, 226, 110, 255) : IM_COL32(242, 244, 248, 255), label);
            if (g_fontSmall) ImGui::PopFont();
        }
    }

    // Target Tracer to currently selected Aimbot enemy
    if (hasAimTarget && g_state.aimTracer != 0) {
        const ImU32 tracerCol = IM_COL32(255, 214, 70, 230);
        bg->AddLine(ImVec2(scrCx, scrCy), ImVec2(aimScrX, aimScrY), IM_COL32(0, 0, 0, 160), 3.0f);
        bg->AddLine(ImVec2(scrCx, scrCy), ImVec2(aimScrX, aimScrY), tracerCol, 1.6f);
        bg->AddCircle(ImVec2(aimScrX, aimScrY), 6.5f * scale, tracerCol, 16, 1.8f);
    }

    // Native 120Hz Aimbot write with Recoil Compensation (subtracts m_RecoilAngles)
    if (hasAimTarget && IsValidUserPtr(snap.playerCameraPtr)) {
        float look[2]{};
        float rec[2]{};
        if (SafeReadMem(snap.playerCameraPtr + g_mwrCamOff.lookAngles, look, sizeof(look)) &&
            SafeReadMem(snap.playerCameraPtr + g_mwrCamOff.recoilAngles, rec, sizeof(rec))) {
            const float dx = aimX - camX;
            const float dy = aimY - camY;
            const float dz = aimZ - camZ;
            const float horiz = std::sqrt(dx * dx + dz * dz);
            if (horiz > 0.01f) {
                constexpr float kRadToDeg = 57.2957795f;
                const float recPitch = needNoRec ? 0.0f : rec[0];
                const float recYaw   = needNoRec ? 0.0f : rec[1];

                const float targetPitch = -std::atan2(dy, horiz) * kRadToDeg - recPitch;
                const float targetYaw   =  std::atan2(dx, dz)    * kRadToDeg - recYaw;

                float dYaw = targetYaw - look[1];
                while (dYaw > 180.0f) dYaw -= 360.0f;
                while (dYaw < -180.0f) dYaw += 360.0f;
                const float dPitch = targetPitch - look[0];

                const float smooth = std::max(1.0f, g_state.aimSmooth);
                const float newPitch = look[0] + dPitch / smooth;
                const float newYaw   = look[1] + dYaw / smooth;
                if (std::isfinite(newPitch) && std::isfinite(newYaw)) {
                    float* lookPtr = reinterpret_cast<float*>(snap.playerCameraPtr + g_mwrCamOff.lookAngles);
                    lookPtr[0] = newPitch;
                    lookPtr[1] = newYaw;
                }
            }
        }
    }
}

static bool HitRect(const RectF& r, float x, float y, float pad) {
    return x >= r.x0 - pad && x <= r.x1 + pad && y >= r.y0 - pad && y <= r.y1 + pad;
}

// ============================================================================
// Exported C ABI (called from Frida JS)
// ============================================================================
extern "C" {

__attribute__((visibility("default")))
ModState* imgui_get_state() {
    return &g_state;
}

__attribute__((visibility("default")))
ModTelemetry* imgui_get_telemetry() {
    return &g_tele;
}

__attribute__((visibility("default")))
EspSnapshot* imgui_get_esp_snapshot() {
    return &g_esp;
}

__attribute__((visibility("default")))
void imgui_toggle_menu() {
    g_menuOpen.store(!g_menuOpen.load());
}

__attribute__((visibility("default")))
void imgui_set_menu_open(int open) {
    g_menuOpen.store(open != 0);
}

__attribute__((visibility("default")))
int imgui_is_menu_open() {
    return g_menuOpen.load() ? 1 : 0;
}

__attribute__((visibility("default")))
int imgui_on_touch(int actionMasked, int pointerId, float x, float y) {
    if (!g_initialized.load() || g_shutdownReq.load()) return 0;

    const bool isDown = (actionMasked == 0 || actionMasked == 5);
    const bool isUp   = (actionMasked == 1 || actionMasked == 6 || actionMasked == 3);
    const bool isMove = (actionMasked == 2);

    bool hit = false;
    {
        std::lock_guard<std::mutex> lk(g_rectMtx);
        const float pad = 12.0f;
        for (const auto& r : g_activeRects) {
            if (HitRect(r, x, y, pad)) {
                hit = true;
                break;
            }
        }
    }

    if (isDown && g_menuPointerId == -1 && hit) {
        g_menuPointerId = pointerId;
    }

    if (g_menuPointerId == pointerId) {
        const int normAct = isDown ? 0 : (isUp ? 1 : (isMove ? 2 : 3));
        {
            std::lock_guard<std::mutex> lk(g_touchMtx);
            if (g_touchQueue.size() < 64) {
                g_touchQueue.push_back({normAct, x, y});
            }
        }
        if (isUp) {
            g_menuPointerId = -1;
        }
        return 1;
    }

    return 0;
}

__attribute__((visibility("default")))
void imgui_on_swap(void* eglDisplay, void* eglSurface) {
    if (g_shutdownReq.load()) {
        if (g_initialized.load()) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui::DestroyContext();
            g_initialized.store(false);
            CloseSafePipe();
            LOGI("Dear ImGui cleanly shut down on GL thread.");
        }
        g_shutdownDone.store(true);
        return;
    }

    EGLDisplay dpy = static_cast<EGLDisplay>(eglDisplay);
    EGLSurface surf = static_cast<EGLSurface>(eglSurface);
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 400 || h < 400) return;

    g_surfaceW.store(w);
    g_surfaceH.store(h);

    if (!g_initialized.load()) {
        {
            std::lock_guard<std::mutex> lk(g_pipeMtx);
            InitSafePipeLocked();
        }
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;

        const float minDim = static_cast<float>(std::min(w, h));
        const float scale = std::clamp(minDim / 450.0f, 1.3f, 2.6f);
        ApplyCustomStyle(scale);

        ImGui_ImplOpenGL3_Init("#version 300 es");
        g_lastFrameTime = std::chrono::steady_clock::now();
        g_initialized.store(true);
        LOGI("Dear ImGui initialized (%dx%d, scale=%.2f)", w, h, scale);
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - g_lastFrameTime).count();
    g_lastFrameTime = now;
    io.DeltaTime = (dt > 0.0f && dt < 0.25f) ? dt : (1.0f / 60.0f);

    if (g_needClearTouchPos) {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        g_needClearTouchPos = false;
    }

    std::vector<TouchEvent> events;
    {
        std::lock_guard<std::mutex> lk(g_touchMtx);
        events.swap(g_touchQueue);
    }
    for (const auto& ev : events) {
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        if (ev.action == 0) {
            io.AddMousePosEvent(ev.x, ev.y);
            io.AddMouseButtonEvent(0, true);
        } else if (ev.action == 2) {
            io.AddMousePosEvent(ev.x, ev.y);
        } else if (ev.action == 1 || ev.action == 3) {
            io.AddMousePosEvent(ev.x, ev.y);
            io.AddMouseButtonEvent(0, false);
            g_needClearTouchPos = true;
        }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    DrawEspAndAimbot(w, h);

    std::vector<RectF> rects;
    rects.reserve(4);
    DrawModUI(w, h, rects);
    {
        std::lock_guard<std::mutex> lk(g_rectMtx);
        g_activeRects.swap(rects);
    }

    ImGui::Render();

    GLint prevViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    glViewport(0, 0, w, h);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
}

__attribute__((visibility("default")))
void imgui_reset_lifecycle() {
    g_fridaControlled.store(true);
    g_shutdownReq.store(false);
    g_shutdownDone.store(false);
    g_menuPointerId = -1;
    g_needClearTouchPos = false;
    g_state.reqKill = 0;
    ResetFramePageCache();
    std::memset(&g_esp, 0, sizeof(g_esp));
    {
        std::lock_guard<std::mutex> lk(g_touchMtx);
        g_touchQueue.clear();
    }
}

__attribute__((visibility("default")))
void imgui_request_shutdown() {
    g_shutdownReq.store(true);
}

__attribute__((visibility("default")))
int imgui_is_shutdown_done() {
    return g_shutdownDone.load() ? 1 : 0;
}

__attribute__((visibility("default")))
void imgui_get_surface_size(int* outW, int* outH) {
    if (outW) *outW = g_surfaceW.load();
    if (outH) *outH = g_surfaceH.load();
}

} // extern "C"

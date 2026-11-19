#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <dirent.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "mwr_shared.h"
#include "external/ANativeWindowCreator.h"
#include "external/ExtMemReader.h"
#include "external/ExtTouch.h"

#define ELOGI(...) __android_log_print(ANDROID_LOG_INFO,  "MwrExt", __VA_ARGS__)
#define ELOGE(...) __android_log_print(ANDROID_LOG_ERROR, "MwrExt", __VA_ARGS__)

namespace {

static std::atomic<bool> g_quit{false};

static void OnSignal(int) {
    g_quit.store(true);
}

static pid_t FindPackagePid(const char* pkg) {
    DIR* dir = opendir("/proc");
    if (!dir) return -1;
    struct dirent* ent = nullptr;
    pid_t found = -1;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
        const pid_t pid = static_cast<pid_t>(std::atoi(ent->d_name));
        if (pid <= 0) continue;
        char path[64], cmd[128]{};
        std::snprintf(path, sizeof(path), "/proc/%d/cmdline", static_cast<int>(pid));
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, cmd, sizeof(cmd) - 1);
        close(fd);
        if (n > 0 && std::strcmp(cmd, pkg) == 0) {
            found = pid;
            break;
        }
    }
    closedir(dir);
    return found;
}

static pid_t FindTargetGamePid(const char* explicitTarget) {
    if (explicitTarget && explicitTarget[0]) {
        if (explicitTarget[0] >= '0' && explicitTarget[0] <= '9') {
            return static_cast<pid_t>(std::atoi(explicitTarget));
        }
        return FindPackagePid(explicitTarget);
    }
    const char* cands[] = {
        "com.vn.miniworld.royale.sparkgame",
        "com.sea.miniworldroyale.sparkgame",
    };
    for (const char* pkg : cands) {
        pid_t p = FindPackagePid(pkg);
        if (p > 0) return p;
    }
    return -1;
}

// ============================================================================
// External Read-Only Config, Telemetry & Lock-Free ESP Snapshot
// ============================================================================
struct ExtConfig {
    bool  aimbot       = false;
    float aimFovPx     = 160.0f;
    float aimSmooth    = 4.0f;
    bool  drawFov      = true;
    bool  aimTeamCheck = true;
    bool  aimVisCheck  = false;
    int   aimPriority  = 0; // 0=Near FOV, 1=Closest, 2=Low HP
    int   aimBone      = 0; // 0=Head, 1=Chest, 2=Body
    bool  aimTracer    = true;

    bool  espEnabled   = true;
    bool  espBox       = true;
    bool  espLine      = true;
    bool  espHp        = true;
    bool  espInfo      = true;
    bool  espTeamCheck = true;
    bool  espVisCheck  = false;
    float espMaxDist   = 250.0f;
};

static ExtConfig g_cfg;

struct ExtState {
    bool menuOpen = false;
    bool reqRescan = false;
    bool pillDragging = false;
    ImVec2 pillPos = ImVec2(24.0f, 36.0f);
    int activeTab = 0; // 0=Visuals, 1=System
};

static ExtState     g_ui;
static ModTelemetry g_tele{};
static EspSnapshot  g_esp{};
static uint32_t     g_offNativeCamVp = 0xdc;

// ============================================================================
// Auto-Updating IL2CPP Class & FieldInfo Offsets (100% Read-Only via pread64)
// ============================================================================
struct ExtOffsets {
    uint32_t klass_parent = 0x58;
    uint32_t klass_fields = 0x80;
    uint32_t klass_static = 0xb8;
    uint32_t obj_cachedPtr = 0x10;

    uint32_t dict_entries = 0x18;
    uint32_t dict_count = 0x20;
    uint32_t arr_len = 0x18;
    uint32_t arr_data = 0x20;
    uint32_t dictEntryStride = 24;
    uint32_t dictEntryVal = 0x10;

    uint32_t pm_localHero = 0x00;
    uint32_t pm_playerDict = 0x18;

    uint32_t lpm_instance = 0x00;
    uint32_t lpm_localPlayer = 0x18;
    uint32_t lpm_bindNetId = 0x20;

    uint32_t cm_instance = 0x00;
    uint32_t cm_camera = 0x18;
    uint32_t cm_cameraTf = 0x28;

    uint32_t np_transform = 0x30;
    uint32_t np_isDestroy = 0x38;
    uint32_t np_shareData = 0xc0;

    uint32_t aps_playerTrans = 0x58;
    uint32_t aps_netData = 0x70;
    uint32_t aps_avatarCtrl = 0x78;
    uint32_t aps_netPlayerId = 0x88;
    uint32_t aps_baseData = 0xa8;

    uint32_t av_mRenderer = 0xd0;
    uint32_t av_renderShow = 0xe0;
    uint32_t ar_rendDict = 0x10;
    uint32_t list_items = 0x10;
    uint32_t list_size = 0x18;

    uint32_t ahs_netData = 0x28;
    uint32_t ahs_baseData = 0x30;
    uint32_t bd_heroTeamId = 0x20;
    uint32_t bd_playerTeamId = 0x20;

    uint32_t npd_playerPos = 0x80;
    uint32_t npd_battle = 0x1d0;
    uint32_t npd_teamIdStr = 0x2e0;
    uint32_t npd_playerNameStr = 0x2f0;

    uint32_t nv_internalValue = 0x48;
    uint32_t bat_maxHp = 0x28;
    uint32_t bat_hp = 0x38;

    uint32_t pl_camera = 0x4e8;
    uint32_t pc_lookAngles = 0x50;
    uint32_t pc_recoilAngles = 0x58;
};

struct ExtClasses {
    uint64_t PlayerManager = 0;
    uint64_t LocalPlayerManager = 0;
    uint64_t CameraManager = 0;
    uint64_t NetPlayerBase = 0;
    uint64_t Hero = 0;
    uint64_t NetPlayerData2 = 0;
    uint64_t Battle = 0;
    uint64_t Player = 0;
    uint64_t PlayerCamera = 0;
    uint64_t ActorPlayerShareData = 0;
    uint64_t ActorHeroShareData = 0;
    uint64_t AvatarRendererComp = 0;
    uint64_t UnityObject = 0;
};

struct PlayerCacheEntry {
    int32_t     netId = -1;
    std::string teamStr;
    std::string name;
    uint64_t    natRend = 0;
    uint64_t    av = 0;
};

static ExtMemReader                   g_mem;
static ExtOffsets                     g_off;
static ExtClasses                     g_cls;
static std::unordered_set<uint64_t>   g_resolvedKlasses;
static std::unordered_map<uint64_t, PlayerCacheEntry> g_playerCache;
static uint64_t                       g_cachedLocalHero = 0;
static std::string                    g_cachedLocalTeamStr;
static uint64_t                       g_metaBase = 0;
static uint32_t                       g_metaStrOff = 0;
static uint32_t                       g_metaStrSize = 0;
static char                           g_statusText[64] = "Scanning IL2CPP...";

static bool ShouldSkipScanRegion(const ExtMemReader::MapsRegion& r) {
    if (r.end <= r.start) return true;
    if (r.path.empty()) return false;
    const char* p = r.path.c_str();
    return std::strstr(p, "[vdso]") != nullptr ||
           std::strstr(p, "[vvar]") != nullptr ||
           std::strstr(p, "[stack") != nullptr ||
           std::strstr(p, "/dev/kgsl") != nullptr ||
           std::strstr(p, "/dev/mali") != nullptr ||
           std::strstr(p, "/dev/ion") != nullptr ||
           std::strstr(p, "/dev/dri") != nullptr ||
           std::strstr(p, "/dmabuf") != nullptr ||
           std::strstr(p, "/vendor/") != nullptr ||
           std::strstr(p, "/system/") != nullptr ||
           std::strstr(p, ".apk") != nullptr ||
           std::strstr(p, ".dex") != nullptr ||
           std::strstr(p, ".oat") != nullptr ||
           std::strstr(p, ".art") != nullptr ||
           std::strstr(p, ".ttf") != nullptr;
}

static bool ValidateMetadataCandidate(uint64_t candBase, uint64_t regionEnd,
                                      uint32_t& outStrOff, uint32_t& outStrSize) {
    if (candBase + 0x40ULL > regionEnd) return false;
    const int32_t ver = g_mem.ReadVal<int32_t>(candBase + 4, 0);
    if (ver < 21 || ver > 35) return false;
    const uint32_t strOff = g_mem.ReadVal<uint32_t>(candBase + 24, 0);
    const uint32_t strSize = g_mem.ReadVal<uint32_t>(candBase + 28, 0);
    if (strOff < 0x1000U || strOff > 0x800000U) return false;
    if (strSize < 0x40000U || strSize > 0x1000000U) return false;
    if (candBase + static_cast<uint64_t>(strOff) + static_cast<uint64_t>(strSize) > regionEnd) return false;

    char sig[9]{};
    if (!g_mem.ReadMem(candBase + strOff, sig, sizeof(sig))) return false;
    if (std::memcmp(sig, "mscorlib", 9) != 0) return false;

    outStrOff = strOff;
    outStrSize = strSize;
    return true;
}

static bool FindMetadataBase(const std::vector<ExtMemReader::MapsRegion>& regions) {
    if (g_metaBase) {
        uint32_t tmpOff = 0, tmpSz = 0;
        if (ValidateMetadataCandidate(g_metaBase, g_metaBase + 0x1600000ULL, tmpOff, tmpSz)) {
            return true;
        }
        g_metaBase = 0;
    }

    // Pass 1: standard decrypted metadata mmap (~0x12c8000, including rw-s /dev/zero)
    for (const auto& r : regions) {
        if (r.perms[0] != 'r' || (r.perms[3] != 'p' && r.perms[3] != 's')) continue;
        if (ShouldSkipScanRegion(r) || !ExtMemReader::IsValidAddrRange(r.start)) continue;
        const uint64_t sz = r.end - r.start;
        if (sz >= 0x1000000ULL && sz <= 0x1600000ULL) {
            if (ValidateMetadataCandidate(r.start, r.end, g_metaStrOff, g_metaStrSize)) {
                g_metaBase = r.start;
                return true;
            }
        }
    }

    // Pass 2: fallback scan inside larger merged VMAs
    std::vector<uint8_t> pageBuf(4096);
    for (const auto& r : regions) {
        if (r.perms[0] != 'r' || (r.perms[3] != 'p' && r.perms[3] != 's')) continue;
        if (ShouldSkipScanRegion(r) || !ExtMemReader::IsValidAddrRange(r.start)) continue;
        if ((r.end - r.start) < 0x1000000ULL) continue;
        for (uint64_t page = r.start; page + 0x1000000ULL <= r.end; page += 4096ULL) {
            const size_t lim = std::min<size_t>(4096U, static_cast<size_t>(r.end - page));
            if (lim < 12 || !g_mem.ReadMem(page, pageBuf.data(), lim)) continue;
            const auto* p = reinterpret_cast<const uint32_t*>(pageBuf.data());
            const size_t n = lim / 4;
            for (size_t i = 1; i + 1 < n; ++i) {
                if (p[i] >= 21U && p[i] <= 35U && p[i + 1] == 256U) {
                    const uint64_t cand = page + i * 4ULL - 4ULL;
                    if (ValidateMetadataCandidate(cand, r.end, g_metaStrOff, g_metaStrSize)) {
                        g_metaBase = cand;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static bool ResolveFieldInSingleClass(uint64_t klass, const char* fieldName, uint32_t& outOff) {
    if (!ExtMemReader::IsValidUserPtr(klass) || !fieldName) return false;
    const uint64_t fieldsPtr = g_mem.ReadPtr(klass + g_off.klass_fields);
    if (!ExtMemReader::IsValidUserPtr(fieldsPtr)) return false;

    const uint64_t klassU = ExtMemReader::Untag(klass);
    for (int i = 0; i < 240; ++i) {
        const uint64_t fi = fieldsPtr + static_cast<uint64_t>(i) * 32ULL;
        uint64_t raw[4]{};
        if (!g_mem.ReadMem(fi, raw, sizeof(raw))) break;
        if (ExtMemReader::Untag(raw[2]) != klassU) break;

        const uint64_t namePtr = raw[0];
        if (!ExtMemReader::IsValidAddrRange(namePtr)) continue;

        char buf[64]{};
        if (!g_mem.ReadMem(namePtr, buf, sizeof(buf) - 1)) continue;
        buf[sizeof(buf) - 1] = '\0';
        if (std::strcmp(buf, fieldName) == 0) {
            const int32_t off = static_cast<int32_t>(raw[3] & 0xffffffffULL);
            if (off >= 0 && off < 0x2000) {
                outOff = static_cast<uint32_t>(off);
                return true;
            }
        }
    }
    return false;
}

static bool ResolveFieldFromChain(uint64_t klass, const char* fieldName, uint32_t& outOff, int maxDepth = 5) {
    uint64_t cur = klass;
    for (int d = 0; d < maxDepth && ExtMemReader::IsValidUserPtr(cur); ++d) {
        if (ResolveFieldInSingleClass(cur, fieldName, outOff)) return true;
        cur = g_mem.ReadPtr(cur + g_off.klass_parent);
    }
    return false;
}

static bool ResolveFieldAlias(uint64_t klass, const char* const* names, size_t nNames, uint32_t& outOff, int maxDepth = 5) {
    for (size_t i = 0; i < nNames; ++i) {
        if (ResolveFieldFromChain(klass, names[i], outOff, maxDepth)) return true;
    }
    return false;
}

static void DetectFieldsOffset(uint64_t klass) {
    if (!ExtMemReader::IsValidUserPtr(klass)) return;
    const uint64_t kU = ExtMemReader::Untag(klass);
    const uint32_t candidates[] = {g_off.klass_fields, 0x80, 0x78, 0x88, 0x70, 0x90, 0x98, 0xa0};
    for (uint32_t cand : candidates) {
        uint64_t fld = g_mem.ReadPtr(klass + cand);
        uint64_t fp = fld ? g_mem.ReadPtr(fld + 0x10) : 0;
        if (fp && ExtMemReader::Untag(fp) == kU) {
            g_off.klass_fields = cand;
            return;
        }
    }
}

static void ApplyAllClassOffsets() {
    if (g_cls.UnityObject) {
        ResolveFieldFromChain(g_cls.UnityObject, "m_CachedPtr", g_off.obj_cachedPtr, 2);
    }
    if (g_cls.PlayerManager) {
        ResolveFieldFromChain(g_cls.PlayerManager, "LocalHero", g_off.pm_localHero, 2);
        ResolveFieldFromChain(g_cls.PlayerManager, "_playerDict", g_off.pm_playerDict, 2);
    }
    if (g_cls.LocalPlayerManager) {
        ResolveFieldFromChain(g_cls.LocalPlayerManager, "_instance", g_off.lpm_instance, 2);
        ResolveFieldFromChain(g_cls.LocalPlayerManager, "mLocalPlayer", g_off.lpm_localPlayer, 2);
        ResolveFieldFromChain(g_cls.LocalPlayerManager, "curBindNetPlayerId", g_off.lpm_bindNetId, 2);
    }
    if (g_cls.CameraManager) {
        ResolveFieldFromChain(g_cls.CameraManager, "instance", g_off.cm_instance, 2);
        ResolveFieldFromChain(g_cls.CameraManager, "mCamera", g_off.cm_camera, 2);
        ResolveFieldFromChain(g_cls.CameraManager, "mCameraTransform", g_off.cm_cameraTf, 2);
    }
    if (g_cls.NetPlayerBase) {
        ResolveFieldFromChain(g_cls.NetPlayerBase, "<mTransform>k__BackingField", g_off.np_transform, 2);
        ResolveFieldFromChain(g_cls.NetPlayerBase, "isDestory", g_off.np_isDestroy, 2);
    }
    if (g_cls.Hero) {
        ResolveFieldFromChain(g_cls.Hero, "<mShareData>k__BackingField", g_off.np_shareData, 2);
    }
    if (g_cls.ActorHeroShareData) {
        ResolveFieldFromChain(g_cls.ActorHeroShareData, "<netData>k__BackingField", g_off.ahs_netData, 2);
        ResolveFieldFromChain(g_cls.ActorHeroShareData, "_baseData", g_off.ahs_baseData, 2);
    }
    if (g_cls.ActorPlayerShareData) {
        ResolveFieldFromChain(g_cls.ActorPlayerShareData, "<playerTrans>k__BackingField", g_off.aps_playerTrans, 2);
        ResolveFieldFromChain(g_cls.ActorPlayerShareData, "<netData>k__BackingField", g_off.aps_netData, 2);
        ResolveFieldFromChain(g_cls.ActorPlayerShareData, "avatarController", g_off.aps_avatarCtrl, 2);
        ResolveFieldFromChain(g_cls.ActorPlayerShareData, "netPlayerId", g_off.aps_netPlayerId, 2);
        ResolveFieldFromChain(g_cls.ActorPlayerShareData, "_baseData", g_off.aps_baseData, 2);
    }
    if (g_cls.AvatarRendererComp) {
        ResolveFieldFromChain(g_cls.AvatarRendererComp, "RendererDict", g_off.ar_rendDict, 2);
    }
    if (g_cls.NetPlayerData2) {
        ResolveFieldFromChain(g_cls.NetPlayerData2, "PlayerPosition", g_off.npd_playerPos, 3);
        ResolveFieldFromChain(g_cls.NetPlayerData2, "battle", g_off.npd_battle, 3);
        ResolveFieldFromChain(g_cls.NetPlayerData2, "<TeamIdStr>k__BackingField", g_off.npd_teamIdStr, 3);
        ResolveFieldFromChain(g_cls.NetPlayerData2, "<PlayerNameStr>k__BackingField", g_off.npd_playerNameStr, 3);
    }
    if (g_cls.Battle) {
        ResolveFieldFromChain(g_cls.Battle, "MaxHp", g_off.bat_maxHp, 2);
        ResolveFieldFromChain(g_cls.Battle, "Hp", g_off.bat_hp, 2);
    }
    if (g_cls.Player) {
        ResolveFieldFromChain(g_cls.Player, "_PlayerCamera", g_off.pl_camera, 4);
    }
    if (g_cls.PlayerCamera) {
        ResolveFieldFromChain(g_cls.PlayerCamera, "m_LookAngles", g_off.pc_lookAngles, 3);
        ResolveFieldFromChain(g_cls.PlayerCamera, "m_RecoilAngles", g_off.pc_recoilAngles, 3);
    }
}

enum class LiveObjKind {
    Dict, List, NetVar, AvatarCtrl, AvatarRend,
    HeroShare, ShareData, HeroBaseData, PlayerBaseData, NetPlayerData, Battle, Player, PlayerCam
};

static void EnsureLiveObjectOffsets(uint64_t obj, LiveObjKind kind) {
    if (!ExtMemReader::IsValidUserPtr(obj)) return;
    const uint64_t klass = g_mem.ReadPtr(obj);
    if (!ExtMemReader::IsValidUserPtr(klass)) return;
    const uint64_t klassU = ExtMemReader::Untag(klass);
    if (g_resolvedKlasses.find(klassU) != g_resolvedKlasses.end()) return;
    g_resolvedKlasses.insert(klassU);

    switch (kind) {
        case LiveObjKind::Dict: {
            const char* eAliases[] = {"entries", "_entries"};
            const char* cAliases[] = {"count", "_count"};
            ResolveFieldAlias(klass, eAliases, 2, g_off.dict_entries, 2);
            ResolveFieldAlias(klass, cAliases, 2, g_off.dict_count, 2);
            break;
        }
        case LiveObjKind::List: {
            const char* iAliases[] = {"_items", "items"};
            const char* sAliases[] = {"_size", "size"};
            ResolveFieldAlias(klass, iAliases, 2, g_off.list_items, 2);
            ResolveFieldAlias(klass, sAliases, 2, g_off.list_size, 2);
            break;
        }
        case LiveObjKind::NetVar: {
            const char* nvAliases[] = {"m_InternalValue", "internalValue"};
            ResolveFieldAlias(klass, nvAliases, 2, g_off.nv_internalValue, 3);
            break;
        }
        case LiveObjKind::AvatarCtrl: {
            const char* mrAliases[] = {"mRenderer", "_mRenderer"};
            ResolveFieldAlias(klass, mrAliases, 2, g_off.av_mRenderer, 3);
            ResolveFieldFromChain(klass, "renderIsShow", g_off.av_renderShow, 3);
            break;
        }
        case LiveObjKind::AvatarRend:
            if (!g_cls.AvatarRendererComp) {
                g_cls.AvatarRendererComp = klass;
                ApplyAllClassOffsets();
            }
            break;
        case LiveObjKind::HeroShare:
            if (!g_cls.ActorHeroShareData) {
                g_cls.ActorHeroShareData = klass;
                ApplyAllClassOffsets();
            }
            break;
        case LiveObjKind::ShareData:
            if (!g_cls.ActorPlayerShareData) {
                g_cls.ActorPlayerShareData = klass;
                ApplyAllClassOffsets();
            }
            break;
        case LiveObjKind::HeroBaseData:
            ResolveFieldFromChain(klass, "TeamId", g_off.bd_heroTeamId, 2);
            break;
        case LiveObjKind::PlayerBaseData:
            ResolveFieldFromChain(klass, "TeamId", g_off.bd_playerTeamId, 2);
            break;
        case LiveObjKind::NetPlayerData:
            if (!g_cls.NetPlayerData2) {
                g_cls.NetPlayerData2 = klass;
                ApplyAllClassOffsets();
            }
            break;
        case LiveObjKind::Battle:
            if (!g_cls.Battle) {
                g_cls.Battle = klass;
                ApplyAllClassOffsets();
            }
            break;
        case LiveObjKind::Player:
            if (!g_cls.Player) {
                g_cls.Player = klass;
                ApplyAllClassOffsets();
            }
            break;
        case LiveObjKind::PlayerCam:
            if (!g_cls.PlayerCamera) {
                g_cls.PlayerCamera = klass;
                ApplyAllClassOffsets();
            }
            break;
    }
}

static void ScanClassesExternal() {
    if (!g_mem.IsAttached()) return;
    const auto regions = g_mem.ReadMaps();
    if (regions.empty()) {
        std::snprintf(g_statusText, sizeof(g_statusText), "No maps (pid %d)", static_cast<int>(g_mem.Pid()));
        return;
    }
    if (!FindMetadataBase(regions)) {
        std::snprintf(g_statusText, sizeof(g_statusText), "No meta (%zu rgns)", regions.size());
        return;
    }

    const uint64_t strBase = g_metaBase + g_metaStrOff;
    const uint64_t strEnd  = strBase + g_metaStrSize;
    const uint64_t strBaseU = ExtMemReader::Untag(strBase);
    const uint64_t strEndU  = ExtMemReader::Untag(strEnd);

    std::vector<uint8_t> strBuf(g_metaStrSize, 0);
    g_mem.ReadChunked(strBase, strBuf.data(), g_metaStrSize);

    struct TargetName {
        const char* name;
        const char* reqField;
        uint64_t*   slot;
        uint64_t    strAddrU;
    };
    const struct {
        const char* name;
        const char* reqField;
        uint64_t*   slot;
    } wanted[] = {
        {"PlayerManager",        "LocalHero",                  &g_cls.PlayerManager},
        {"LocalPlayerManager",   "_instance",                  &g_cls.LocalPlayerManager},
        {"CameraManager",        "mCamera",                    &g_cls.CameraManager},
        {"NetPlayerBase",        "isDestory",                  &g_cls.NetPlayerBase},
        {"Hero",                 "<mShareData>k__BackingField", &g_cls.Hero},
        {"NetPlayerData2",       "PlayerPosition",             &g_cls.NetPlayerData2},
        {"Battle",               "MaxHp",                      &g_cls.Battle},
        {"Player",               "_PlayerCamera",              &g_cls.Player},
        {"PlayerCamera",         "m_LookAngles",               &g_cls.PlayerCamera},
        {"ActorPlayerShareData", "avatarController",           &g_cls.ActorPlayerShareData},
        {"ActorHeroShareData",   "<netData>k__BackingField",    &g_cls.ActorHeroShareData},
        {"AvatarRendererComp",   "RendererDict",               &g_cls.AvatarRendererComp},
        {"Object",               "m_CachedPtr",                &g_cls.UnityObject},
    };

    std::vector<TargetName> targets;
    for (const auto& w : wanted) {
        if (*w.slot != 0) continue;
        const size_t nlen = std::strlen(w.name);
        for (size_t i = 1; i + nlen + 1 <= strBuf.size(); ++i) {
            if (strBuf[i - 1] == 0 && strBuf[i + nlen] == 0 &&
                std::memcmp(strBuf.data() + i, w.name, nlen) == 0) {
                targets.push_back({w.name, w.reqField, w.slot, ExtMemReader::Untag(strBase + i)});
            }
        }
    }
    if (targets.empty()) {
        ApplyAllClassOffsets();
        return;
    }

    bool fieldsOffsetProbed = false;
    std::vector<uint8_t> pageBuf(4096);
    for (const auto& r : regions) {
        if (r.perms[0] != 'r' || r.perms[1] != 'w' || r.perms[3] != 'p') continue;
        if (ShouldSkipScanRegion(r) || !ExtMemReader::IsValidAddrRange(r.start)) continue;
        if ((r.end - r.start) < 0x10000ULL) continue;

        for (uint64_t page = r.start; page < r.end; page += 4096ULL) {
            const size_t lim = std::min<size_t>(4096U, static_cast<size_t>(r.end - page));
            if (!g_mem.ReadMem(page, pageBuf.data(), lim)) continue;
            const size_t nQ = lim / sizeof(uint64_t);
            const auto* q = reinterpret_cast<const uint64_t*>(pageBuf.data());

            for (size_t i = 0; i < nQ; ++i) {
                const uint64_t u = ExtMemReader::Untag(q[i]);
                if (u < strBaseU || u >= strEndU) continue;

                for (const auto& t : targets) {
                    if (*(t.slot) != 0 || u != t.strAddrU) continue;
                    const uint64_t nameFieldAddr = page + i * sizeof(uint64_t);
                    if (nameFieldAddr < 0x10ULL) continue;
                    const uint64_t k = nameFieldAddr - 0x10ULL;

                    const uint64_t img = g_mem.ReadPtr(k + 0x00);
                    uint64_t ns = 0;
                    g_mem.ReadMem(k + 0x18, &ns, sizeof(ns));
                    const bool nsOk = (ExtMemReader::Untag(ns) >= strBaseU && ExtMemReader::Untag(ns) < strEndU);
                    if (!nsOk || !img) continue;

                    if (!fieldsOffsetProbed) {
                        DetectFieldsOffset(k);
                        fieldsOffsetProbed = true;
                    }
                    const uint64_t st = g_mem.ReadPtr(k + g_off.klass_static);
                    if (t.reqField) {
                        uint32_t dummyOff = 0;
                        if (!ResolveFieldFromChain(k, t.reqField, dummyOff, 4) && !st) {
                            continue;
                        }
                    } else if (!st) {
                        uint64_t fields = g_mem.ReadPtr(k + g_off.klass_fields);
                        uint64_t fParent = fields ? g_mem.ReadPtr(fields + 0x10) : 0;
                        if (!fParent || ExtMemReader::Untag(fParent) != ExtMemReader::Untag(k)) continue;
                    }
                    *(t.slot) = k;
                }
            }

            bool allDone = true;
            for (const auto& t : targets) {
                if (*(t.slot) == 0) { allDone = false; break; }
            }
            if (allDone) {
                ApplyAllClassOffsets();
                return;
            }
        }
    }
    ApplyAllClassOffsets();
}

static std::string ReadIl2CppString(uint64_t strPtr, int maxLen = 32) {
    if (!ExtMemReader::IsValidUserPtr(strPtr)) return {};
    const int32_t len = g_mem.ReadVal<int32_t>(strPtr + 0x10, 0);
    if (len <= 0 || len > 128) return {};
    const int n = std::min(len, maxLen);
    uint16_t buf[64]{};
    if (!g_mem.ReadMem(strPtr + 0x14, buf, static_cast<size_t>(n) * 2)) return {};
    std::string out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        const uint16_t ch = buf[i];
        if (ch >= 0x20 && ch <= 0x7e) out.push_back(static_cast<char>(ch));
        else if (ch == 0x0111) out.push_back('d');
        else if (ch == 0x0110) out.push_back('D');
    }
    return out;
}

static std::string SanitizeName(const std::string& raw, int32_t netId) {
    size_t start = 0;
    while (start < raw.size() && raw[start] == ' ') ++start;
    size_t end = raw.size();
    while (end > start && raw[end - 1] == ' ') --end;
    if (end - start >= 2) {
        return raw.substr(start, std::min<size_t>(end - start, 19));
    }
    char buf[20];
    if (netId > 0) std::snprintf(buf, sizeof(buf), "Enemy #%d", netId);
    else std::snprintf(buf, sizeof(buf), "Enemy");
    return buf;
}

static std::string ReadPlayerTeamString(uint64_t share, uint64_t npd, bool isHero) {
    if (share) {
        const uint64_t baseOff = isHero ? g_off.ahs_baseData : g_off.aps_baseData;
        const uint64_t baseData = g_mem.ReadPtr(share + baseOff);
        if (baseData) {
            EnsureLiveObjectOffsets(baseData, isHero ? LiveObjKind::HeroBaseData : LiveObjKind::PlayerBaseData);
            const uint32_t teamOff = isHero ? g_off.bd_heroTeamId : g_off.bd_playerTeamId;
            std::string s = ReadIl2CppString(g_mem.ReadPtr(baseData + teamOff), 64);
            if (!s.empty()) return s;
        }
    }
    if (npd) {
        return ReadIl2CppString(g_mem.ReadPtr(npd + g_off.npd_teamIdStr), 64);
    }
    return {};
}

static void ResolvePlayerRenderer(uint64_t share, uint64_t& outNatRend, uint64_t& outAv) {
    outNatRend = 0;
    outAv = 0;
    if (!ExtMemReader::IsValidUserPtr(share)) return;
    outAv = g_mem.ReadPtr(share + g_off.aps_avatarCtrl);
    if (!outAv) return;
    EnsureLiveObjectOffsets(outAv, LiveObjKind::AvatarCtrl);
    const uint64_t mRend = g_mem.ReadPtr(outAv + g_off.av_mRenderer);
    if (mRend) EnsureLiveObjectOffsets(mRend, LiveObjKind::AvatarRend);
    const uint64_t rDict = mRend ? g_mem.ReadPtr(mRend + g_off.ar_rendDict) : 0;
    if (rDict) EnsureLiveObjectOffsets(rDict, LiveObjKind::Dict);
    const uint64_t re = rDict ? g_mem.ReadPtr(rDict + g_off.dict_entries) : 0;
    if (!re) return;

    const int32_t arrLen = g_mem.ReadVal<int32_t>(re + g_off.arr_len, 0);
    const int32_t scan = std::clamp(arrLen, 0, 10);
    for (int32_t j = 0; j < scan; ++j) {
        const uint64_t reE = re + g_off.arr_data + static_cast<uint64_t>(j) * g_off.dictEntryStride;
        if (g_mem.ReadVal<int32_t>(reE, -1) < 0) continue;
        const uint64_t lst = g_mem.ReadPtr(reE + g_off.dictEntryVal);
        if (!lst) continue;
        EnsureLiveObjectOffsets(lst, LiveObjKind::List);
        const int32_t sz = g_mem.ReadVal<int32_t>(lst + g_off.list_size, 0);
        if (sz <= 0) continue;
        const uint64_t items = g_mem.ReadPtr(lst + g_off.list_items);
        if (!items) continue;
        const uint64_t rend0 = g_mem.ReadPtr(items + g_off.arr_data);
        const uint64_t natRend = rend0 ? g_mem.ReadPtr(rend0 + g_off.obj_cachedPtr) : 0;
        if (natRend) {
            outNatRend = natRend;
            return;
        }
    }
}

static int RunExternalReaderTick() {
    if (!g_mem.IsAttached()) return 250;

    if (g_ui.reqRescan) {
        g_ui.reqRescan = false;
        g_cls = ExtClasses{};
        g_resolvedKlasses.clear();
        g_playerCache.clear();
        g_cachedLocalHero = 0;
        g_cachedLocalTeamStr.clear();
        ScanClassesExternal();
    }

    static auto s_lastScan = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    if (!g_cls.PlayerManager || !g_cls.CameraManager || !g_cls.LocalPlayerManager) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastScan).count() >= 2500) {
            s_lastScan = now;
            ScanClassesExternal();
        }
    }

    uint64_t camMgrInst = 0, nativeCam = 0, natCamTf = 0;
    if (g_cls.CameraManager) {
        const uint64_t cmSt = g_mem.ReadPtr(g_cls.CameraManager + g_off.klass_static);
        camMgrInst = cmSt ? g_mem.ReadPtr(cmSt + g_off.cm_instance) : 0;
        if (camMgrInst) {
            const uint64_t mCam = g_mem.ReadPtr(camMgrInst + g_off.cm_camera);
            if (mCam) nativeCam = g_mem.ReadPtr(mCam + g_off.obj_cachedPtr);
            const uint64_t mCamTf = g_mem.ReadPtr(camMgrInst + g_off.cm_cameraTf);
            if (mCamTf) natCamTf = g_mem.ReadPtr(mCamTf + g_off.obj_cachedPtr);
        }
    }

    uint64_t lpmInst = 0, localPlayer = 0, playerCam = 0, localHero = 0;
    int32_t localNetId = -1;
    float localHp = 0.0f, localMaxHp = 0.0f;

    if (g_cls.LocalPlayerManager) {
        const uint64_t lpmSt = g_mem.ReadPtr(g_cls.LocalPlayerManager + g_off.klass_static);
        lpmInst = lpmSt ? g_mem.ReadPtr(lpmSt + g_off.lpm_instance) : 0;
        if (lpmInst) {
            localPlayer = g_mem.ReadPtr(lpmInst + g_off.lpm_localPlayer);
            localNetId = g_mem.ReadVal<int32_t>(lpmInst + g_off.lpm_bindNetId, -1);
            if (localPlayer) {
                EnsureLiveObjectOffsets(localPlayer, LiveObjKind::Player);
                playerCam = g_mem.ReadPtr(localPlayer + g_off.pl_camera);
                if (playerCam) {
                    EnsureLiveObjectOffsets(playerCam, LiveObjKind::PlayerCam);
                }
            }
        }
    }

    const bool needEntities = (g_cfg.espEnabled || g_cfg.aimbot || g_ui.menuOpen);
    uint64_t pmStatic = 0;
    int32_t dictCount = 0, enemyCount = 0;
    static EspEntity s_ents[kMaxEspEntities];
    int entOutCount = 0;

    static uint32_t s_teamRefreshTick = 0;
    ++s_teamRefreshTick;
    const bool periodicTeamRefresh = (s_teamRefreshTick % 16U) == 0;

    if (g_cls.PlayerManager) {
        pmStatic = g_mem.ReadPtr(g_cls.PlayerManager + g_off.klass_static);
        if (pmStatic) {
            localHero = g_mem.ReadPtr(pmStatic + g_off.pm_localHero);
            if (localHero) {
                const uint64_t hShare = g_mem.ReadPtr(localHero + g_off.np_shareData);
                if (hShare) EnsureLiveObjectOffsets(hShare, LiveObjKind::HeroShare);
                const uint64_t hNpd = hShare ? g_mem.ReadPtr(hShare + g_off.ahs_netData) : 0;
                if (hNpd) EnsureLiveObjectOffsets(hNpd, LiveObjKind::NetPlayerData);

                if (localHero != g_cachedLocalHero) {
                    g_cachedLocalHero = localHero;
                    g_cachedLocalTeamStr = ReadPlayerTeamString(hShare, hNpd, true);
                    g_playerCache.clear();
                } else if (g_cachedLocalTeamStr.empty() || periodicTeamRefresh) {
                    std::string latest = ReadPlayerTeamString(hShare, hNpd, true);
                    if (!latest.empty()) g_cachedLocalTeamStr = std::move(latest);
                }

                if (hNpd && g_ui.menuOpen) {
                    const uint64_t bat = g_mem.ReadPtr(hNpd + g_off.npd_battle);
                    if (bat) {
                        EnsureLiveObjectOffsets(bat, LiveObjKind::Battle);
                        const uint64_t hpV = g_mem.ReadPtr(bat + g_off.bat_hp);
                        const uint64_t mhpV = g_mem.ReadPtr(bat + g_off.bat_maxHp);
                        if (hpV) {
                            EnsureLiveObjectOffsets(hpV, LiveObjKind::NetVar);
                            localHp = static_cast<float>(g_mem.ReadVal<int32_t>(hpV + g_off.nv_internalValue, 0));
                        }
                        if (mhpV) {
                            localMaxHp = static_cast<float>(g_mem.ReadVal<int32_t>(mhpV + g_off.nv_internalValue, 0));
                        }
                    }
                }
            } else if (g_cachedLocalHero != 0) {
                g_cachedLocalHero = 0;
                g_cachedLocalTeamStr.clear();
                g_playerCache.clear();
            }

            const uint64_t dict = g_mem.ReadPtr(pmStatic + g_off.pm_playerDict);
            if (dict) {
                EnsureLiveObjectOffsets(dict, LiveObjKind::Dict);
                dictCount = g_mem.ReadVal<int32_t>(dict + g_off.dict_count, 0);
                if (needEntities && dictCount > 0 && dictCount < 256) {
                    const uint64_t entries = g_mem.ReadPtr(dict + g_off.dict_entries);
                    if (entries) {
                        const int32_t arrLen = g_mem.ReadVal<int32_t>(entries + g_off.arr_len, 0);
                        const int32_t scanLen = std::clamp(arrLen, 0, 200);
                        for (int32_t i = 0; i < scanLen; ++i) {
                            const uint64_t entAddr = entries + g_off.arr_data + static_cast<uint64_t>(i) * g_off.dictEntryStride;
                            if (g_mem.ReadVal<int32_t>(entAddr, -1) < 0) continue;
                            const uint64_t netPlayer = g_mem.ReadPtr(entAddr + g_off.dictEntryVal);
                            if (!netPlayer || netPlayer == localHero) continue;
                            if (g_mem.ReadVal<uint8_t>(netPlayer + g_off.np_isDestroy, 1) != 0) continue;

                            const uint64_t share = g_mem.ReadPtr(netPlayer + g_off.np_shareData);
                            if (share) EnsureLiveObjectOffsets(share, LiveObjKind::ShareData);
                            const uint64_t npd = share ? g_mem.ReadPtr(share + g_off.aps_netData) : 0;
                            if (npd) EnsureLiveObjectOffsets(npd, LiveObjKind::NetPlayerData);

                            auto it = g_playerCache.find(netPlayer);
                            if (it == g_playerCache.end()) {
                                PlayerCacheEntry ce;
                                ce.netId = share ? g_mem.ReadVal<int32_t>(share + g_off.aps_netPlayerId, -1) : -1;
                                ce.teamStr = ReadPlayerTeamString(share, npd, false);
                                std::string rawName;
                                if (npd) {
                                    rawName = ReadIl2CppString(g_mem.ReadPtr(npd + g_off.npd_playerNameStr), 32);
                                }
                                ce.name = SanitizeName(rawName, ce.netId);
                                ResolvePlayerRenderer(share, ce.natRend, ce.av);
                                it = g_playerCache.emplace(netPlayer, std::move(ce)).first;
                            } else {
                                if (it->second.teamStr.empty() || periodicTeamRefresh) {
                                    std::string latest = ReadPlayerTeamString(share, npd, false);
                                    if (!latest.empty()) it->second.teamStr = std::move(latest);
                                }
                                if (!it->second.natRend) {
                                    ResolvePlayerRenderer(share, it->second.natRend, it->second.av);
                                }
                            }

                            const uint8_t renderShow = it->second.av
                                ? g_mem.ReadVal<uint8_t>(it->second.av + g_off.av_renderShow, 1)
                                : 1;

                            if (localNetId > 0 && it->second.netId == localNetId) continue;

                            uint64_t mTf = g_mem.ReadPtr(netPlayer + g_off.np_transform);
                            if (!mTf && share) mTf = g_mem.ReadPtr(share + g_off.aps_playerTrans);
                            const uint64_t natTf = mTf ? g_mem.ReadPtr(mTf + g_off.obj_cachedPtr) : 0;

                            float hp = 100.0f, maxHp = 100.0f;
                            float fx = 0.0f, fy = 0.0f, fz = 0.0f;
                            if (npd) {
                                const uint64_t bat = g_mem.ReadPtr(npd + g_off.npd_battle);
                                if (bat) {
                                    EnsureLiveObjectOffsets(bat, LiveObjKind::Battle);
                                    const uint64_t hpV = g_mem.ReadPtr(bat + g_off.bat_hp);
                                    const uint64_t mhpV = g_mem.ReadPtr(bat + g_off.bat_maxHp);
                                    if (hpV) {
                                        EnsureLiveObjectOffsets(hpV, LiveObjKind::NetVar);
                                        hp = static_cast<float>(g_mem.ReadVal<int32_t>(hpV + g_off.nv_internalValue, 100));
                                    }
                                    if (mhpV) {
                                        maxHp = static_cast<float>(g_mem.ReadVal<int32_t>(mhpV + g_off.nv_internalValue, 100));
                                    }
                                }
                                if (!natTf) {
                                    const uint64_t pv = g_mem.ReadPtr(npd + g_off.npd_playerPos);
                                    if (pv) {
                                        EnsureLiveObjectOffsets(pv, LiveObjKind::NetVar);
                                        fx = g_mem.ReadVal<float>(pv + g_off.nv_internalValue, 0.0f);
                                        fy = g_mem.ReadVal<float>(pv + g_off.nv_internalValue + 4, 0.0f);
                                        fz = g_mem.ReadVal<float>(pv + g_off.nv_internalValue + 8, 0.0f);
                                    }
                                }
                            }

                            const bool isEnemy = (!g_cachedLocalTeamStr.empty() && !it->second.teamStr.empty())
                                ? (it->second.teamStr != g_cachedLocalTeamStr)
                                : true;
                            if (isEnemy) ++enemyCount;

                            if (entOutCount < kMaxEspEntities) {
                                EspEntity& dst = s_ents[entOutCount++];
                                dst.nativeTfPtr = natTf;
                                dst.nativeRendPtr = it->second.natRend;
                                dst.x = fx;
                                dst.y = fy;
                                dst.z = fz;
                                dst.hp = hp;
                                dst.maxHp = maxHp;
                                dst.isEnemy = isEnemy ? 1 : 0;
                                dst.renderShow = renderShow ? 1 : 0;
                                std::memset(dst.name, 0, sizeof(dst.name));
                                std::strncpy(dst.name, it->second.name.c_str(), sizeof(dst.name) - 1);
                            }
                        }
                    }
                }
            }
        }
    }

    if (g_cfg.espEnabled || g_cfg.aimbot) {
        const uint32_t curSeq = __atomic_load_n(&g_esp.seq, __ATOMIC_RELAXED);
        const uint32_t busySeq = (curSeq + 1u) | 1u;
        __atomic_store_n(&g_esp.seq, busySeq, __ATOMIC_RELEASE);
        g_esp.count = entOutCount;
        g_esp.nativeCamPtr = nativeCam;
        g_esp.playerCameraPtr = playerCam;
        g_esp.camTfPtr = natCamTf;
        if (entOutCount > 0) {
            std::memcpy(g_esp.entities, s_ents, static_cast<size_t>(entOutCount) * sizeof(EspEntity));
        }
        __atomic_store_n(&g_esp.seq, busySeq + 1u, __ATOMIC_RELEASE);
    }

    g_tele.playerMgrAddr = pmStatic;
    g_tele.localHeroAddr = localHero;
    g_tele.localPlayerAddr = localPlayer;
    g_tele.camMgrAddr = camMgrInst;
    g_tele.nativeCamAddr = nativeCam;
    g_tele.playerCount = dictCount;
    g_tele.enemyCount = enemyCount;
    g_tele.localHp = localHp;
    g_tele.localMaxHp = localMaxHp;
    g_tele.localTeamId = localNetId;
    const char* status = localPlayer ? "Active in match"
                       : (camMgrInst ? "Waiting for match"
                       : (g_cls.CameraManager ? "IL2CPP ready (wait)" : g_statusText));
    std::memset(g_tele.statusText, 0, sizeof(g_tele.statusText));
    std::strncpy(g_tele.statusText, status, sizeof(g_tele.statusText) - 1);

    return (localPlayer && (g_cfg.espEnabled || g_cfg.aimbot)) ? 16 : 120;
}

static void* ReaderThread(void* arg) {
    const char* targetArg = static_cast<const char*>(arg);
    while (!g_quit.load()) {
        if (!g_mem.IsAttached()) {
            const pid_t pid = FindTargetGamePid(targetArg);
            if (pid > 0 && g_mem.Attach(pid)) {
                g_cls = ExtClasses{};
                g_resolvedKlasses.clear();
                g_playerCache.clear();
                g_metaBase = 0;
                ScanClassesExternal();
            } else {
                std::snprintf(g_tele.statusText, sizeof(g_tele.statusText), "Waiting for game...");
                usleep(300000);
                continue;
            }
        } else {
            char procCheck[48];
            std::snprintf(procCheck, sizeof(procCheck), "/proc/%d", static_cast<int>(g_mem.Pid()));
            if (access(procCheck, F_OK) != 0) {
                g_mem.Close();
                g_cls = ExtClasses{};
                g_resolvedKlasses.clear();
                g_playerCache.clear();
                g_esp.count = 0;
                g_tele = ModTelemetry{};
                continue;
            }
        }

        const int delayMs = RunExternalReaderTick();
        usleep(static_cast<useconds_t>(std::max(4, delayMs)) * 1000U);
    }
    g_mem.Close();
    return nullptr;
}

// ============================================================================
// Read-Only Unity Transform + Camera VP + Overlay Rendering
// ============================================================================
static bool IsUnityRendererVisible(uint64_t natRend) {
    if (!ExtMemReader::IsValidUserPtr(natRend)) return true;
    uint8_t flag129 = 0;
    int32_t visIdx = -1;
    if (!g_mem.ReadMem(natRend + 0x129, &flag129, 1)) return true;
    if ((flag129 & 0x10) == 0) return false;
    if (!g_mem.ReadMem(natRend + 0x168, &visIdx, sizeof(visIdx))) return true;
    return visIdx != -1;
}

static bool ReadUnityTransformPos(uint64_t nativeTf, float& ox, float& oy, float& oz) {
    uint64_t tfAccess = 0;
    int32_t idx = -1;
    if (!g_mem.ReadMem(nativeTf + 0x38, &tfAccess, sizeof(tfAccess)) ||
        !g_mem.ReadMem(nativeTf + 0x40, &idx, sizeof(idx))) return false;
    if (!ExtMemReader::IsValidUserPtr(tfAccess) || idx < 0 || idx > 65536) return false;

    uint64_t pMatrices = 0, pIndices = 0;
    if (!g_mem.ReadMem(tfAccess + 0x18, &pMatrices, sizeof(pMatrices)) ||
        !g_mem.ReadMem(tfAccess + 0x20, &pIndices, sizeof(pIndices))) return false;
    if (!ExtMemReader::IsValidUserPtr(pMatrices) || !ExtMemReader::IsValidUserPtr(pIndices)) return false;

    struct TRS {
        float px, py, pz, pw;
        float qx, qy, qz, qw;
        float sx, sy, sz, sw;
    };

    TRS trs{};
    if (!g_mem.ReadMem(pMatrices + static_cast<uint64_t>(idx) * 48ULL, &trs, sizeof(trs))) return false;
    float rx = trs.px, ry = trs.py, rz = trs.pz;

    int32_t cur = -1;
    if (!g_mem.ReadMem(pIndices + static_cast<uint64_t>(idx) * 4ULL, &cur, sizeof(cur))) return false;

    int depth = 0;
    while (cur >= 0 && cur < 65536 && depth < 48) {
        if (!g_mem.ReadMem(pMatrices + static_cast<uint64_t>(cur) * 48ULL, &trs, sizeof(trs))) return false;
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

        if (!g_mem.ReadMem(pIndices + static_cast<uint64_t>(cur) * 4ULL, &cur, sizeof(cur))) break;
        ++depth;
    }
    if (!std::isfinite(rx) || !std::isfinite(ry) || !std::isfinite(rz)) return false;
    ox = rx; oy = ry; oz = rz;
    return true;
}

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

static void DrawExternalEspAndAim(int sw, int sh) {
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    const float scale = std::clamp(std::min(sw, sh) / 450.0f, 1.2f, 2.2f);
    const float scrCx = sw * 0.5f;
    const float scrCy = sh * 0.5f;

    if (!g_cfg.aimbot && g_mem.IsWriteEnabled()) {
        g_mem.CloseWrite();
    }

    if (g_cfg.aimbot && g_cfg.drawFov) {
        bg->AddCircle(ImVec2(scrCx, scrCy), g_cfg.aimFovPx,
                      IM_COL32(99, 176, 250, 170), 64, 1.5f);
    }

    if (!g_cfg.espEnabled && !g_cfg.aimbot) return;

    EspSnapshot snap{};
    for (int retry = 0; retry < 3; ++retry) {
        const uint32_t s1 = __atomic_load_n(&g_esp.seq, __ATOMIC_ACQUIRE);
        if (s1 & 1U) continue;
        std::memcpy(&snap, &g_esp, sizeof(EspSnapshot));
        const uint32_t s2 = __atomic_load_n(&g_esp.seq, __ATOMIC_ACQUIRE);
        if (s1 == s2) break;
    }
    if (snap.count <= 0 || snap.count > kMaxEspEntities) return;

    float vp[16]{};
    if (!ExtMemReader::IsValidUserPtr(snap.nativeCamPtr)) return;
    if (!g_mem.ReadMem(snap.nativeCamPtr + g_offNativeCamVp, vp, sizeof(vp))) return;
    if (std::fabs(vp[0]) < 1e-4f && std::fabs(vp[5]) < 1e-4f) {
        bool probed = false;
        for (uint32_t probe = 0xc0; probe <= 0x130; probe += 4) {
            float cand[16]{};
            if (g_mem.ReadMem(snap.nativeCamPtr + probe, cand, sizeof(cand)) &&
                std::isfinite(cand[0]) && std::isfinite(cand[5]) && std::isfinite(cand[15]) &&
                cand[0] > 0.2f && cand[0] < 10.0f && cand[5] > 0.2f && cand[5] < 10.0f &&
                std::fabs(cand[3]) <= 1.5f && std::fabs(cand[7]) <= 1.5f &&
                std::fabs(cand[11]) <= 1.5f && std::fabs(cand[15]) > 0.01f) {
                std::memcpy(vp, cand, sizeof(vp));
                g_offNativeCamVp = probe;
                probed = true;
                break;
            }
        }
        if (!probed) return;
    }

    float camX = snap.camPos[0], camY = snap.camPos[1], camZ = snap.camPos[2];
    if (ExtMemReader::IsValidUserPtr(snap.camTfPtr)) {
        float cx, cy, cz;
        if (ReadUnityTransformPos(snap.camTfPtr, cx, cy, cz)) {
            camX = cx; camY = cy; camZ = cz;
        }
    }

    const float fovR2 = g_cfg.aimFovPx * g_cfg.aimFovPx;
    const float boneOffY = (g_cfg.aimBone == 0) ? 1.48f : ((g_cfg.aimBone == 1) ? 1.15f : 0.85f);
    const int prio = std::clamp(g_cfg.aimPriority, 0, 2);

    float bestScore = 1e30f;
    float bestTie = 1e30f;
    bool hasAimTarget = false;
    float aimX = 0.0f, aimY = 0.0f, aimZ = 0.0f;
    float aimScrX = 0.0f, aimScrY = 0.0f;
    int aimTargetIdx = -1;

    for (int i = 0; i < snap.count; ++i) {
        const EspEntity& ent = snap.entities[i];
        if (ent.hp <= 0.0f && ent.maxHp > 0.0f) continue;

        float wx = ent.x, wy = ent.y, wz = ent.z;
        if (ExtMemReader::IsValidUserPtr(ent.nativeTfPtr)) {
            float tx, ty, tz;
            if (ReadUnityTransformPos(ent.nativeTfPtr, tx, ty, tz)) {
                wx = tx; wy = ty; wz = tz;
            }
        }
        const float dx = wx - camX, dy = wy - camY, dz = wz - camZ;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

        bool visComputed = false;
        bool visVal = true;
        auto getVis = [&]() -> bool {
            if (!visComputed) {
                visVal = (ent.renderShow != 0 && IsUnityRendererVisible(ent.nativeRendPtr));
                visComputed = true;
            }
            return visVal;
        };

        if (g_cfg.aimbot && (!g_cfg.aimTeamCheck || ent.isEnemy)) {
            float ax = 0.0f, ay = 0.0f;
            const float targetAimY = wy + boneOffY;
            if (WorldToScreen(vp, wx, targetAimY, wz, sw, sh, ax, ay)) {
                const float adx = ax - scrCx;
                const float ady = ay - scrCy;
                const float d2 = adx * adx + ady * ady;
                if (d2 <= fovR2) {
                    const bool aimVisOk = !g_cfg.aimVisCheck ||
                        (ax >= 0.0f && ax <= static_cast<float>(sw) &&
                         ay >= 0.0f && ay <= static_cast<float>(sh) && getVis());
                    if (aimVisOk) {
                        float score = d2;
                        if (prio == 1) score = dist;
                        else if (prio == 2) score = (ent.hp > 0.0f) ? ent.hp : 9999.0f;

                        if (score < bestScore - 0.5f || (std::fabs(score - bestScore) <= 0.5f && d2 < bestTie)) {
                            bestScore = score;
                            bestTie = d2;
                            hasAimTarget = true;
                            aimX = wx; aimY = targetAimY; aimZ = wz;
                            aimScrX = ax; aimScrY = ay;
                            aimTargetIdx = i;
                        }
                    }
                }
            }
        }

        if (!g_cfg.espEnabled) continue;
        if (g_cfg.espTeamCheck && !ent.isEnemy) continue;
        if (dist > g_cfg.espMaxDist || dist < 0.8f) continue;

        const bool vis = (!g_cfg.espVisCheck) || getVis();

        float headX = 0, headY = 0, footX = 0, footY = 0;
        if (!WorldToScreen(vp, wx, wy + 1.85f, wz, sw, sh, headX, headY)) continue;
        if (!WorldToScreen(vp, wx, wy, wz, sw, sh, footX, footY)) continue;

        const float boxH = std::max(14.0f, footY - headY);
        if (boxH < 4.0f || boxH > static_cast<float>(sh) * 1.4f) continue;
        const float boxW = boxH * 0.52f;
        const float x0 = footX - boxW * 0.5f;
        const float x1 = footX + boxW * 0.5f;
        const float y0 = headY;
        const float y1 = footY;
        if (x1 < -50.0f || x0 > sw + 50.0f || y1 < -50.0f || y0 > sh + 50.0f) continue;

        const bool isLocked = (hasAimTarget && i == aimTargetIdx);
        const ImU32 boxCol = !ent.isEnemy ? IM_COL32(76, 175, 115, 220)
                           : (isLocked ? IM_COL32(255, 214, 70, 255)
                           : (vis ? IM_COL32(235, 75, 75, 240) : IM_COL32(255, 165, 65, 200)));

        if (g_cfg.espLine) {
            bg->AddLine(ImVec2(scrCx, 2.0f), ImVec2(headX, y0), boxCol & 0x7FFFFFFF, 1.2f);
        }
        if (g_cfg.espBox) {
            bg->AddRect(ImVec2(x0 - 1.0f, y0 - 1.0f), ImVec2(x1 + 1.0f, y1 + 1.0f), IM_COL32(0, 0, 0, 170), 2.0f, 0, 2.5f);
            bg->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), boxCol, 2.0f, 0, 1.5f);
        }
        if (g_cfg.espHp) {
            const float ratio = std::clamp((ent.maxHp > 0.0f) ? (ent.hp / ent.maxHp) : 1.0f, 0.0f, 1.0f);
            const float barW = std::max(3.0f, 2.8f * scale);
            const float bx1 = x0 - 4.0f;
            const float bx0 = bx1 - barW;
            bg->AddRectFilled(ImVec2(bx0 - 1.0f, y0 - 1.0f), ImVec2(bx1 + 1.0f, y1 + 1.0f), IM_COL32(10, 12, 16, 200), 1.0f);
            const ImU32 hpCol = (ratio > 0.5f) ? IM_COL32(72, 220, 120, 255)
                              : (ratio > 0.25f) ? IM_COL32(245, 185, 60, 255) : IM_COL32(240, 75, 75, 255);
            const float fillTop = y1 - boxH * ratio;
            if (fillTop < y1) {
                bg->AddRectFilled(ImVec2(bx0, fillTop), ImVec2(bx1, y1), hpCol, 1.0f);
            }
        }
        if (g_cfg.espInfo) {
            char label[64];
            if (ent.name[0]) std::snprintf(label, sizeof(label), "%s [%.0fm]", ent.name, dist);
            else std::snprintf(label, sizeof(label), "Player [%.0fm]", dist);
            const ImVec2 tSz = ImGui::CalcTextSize(label);
            const float tx = headX - tSz.x * 0.5f;
            const float ty = y0 - tSz.y - 4.0f * scale;
            bg->AddText(ImVec2(tx + 1.0f, ty + 1.0f), IM_COL32(0, 0, 0, 210), label);
            bg->AddText(ImVec2(tx, ty), isLocked ? IM_COL32(255, 226, 110, 255) : IM_COL32(242, 244, 248, 255), label);
        }
    }

    if (hasAimTarget && g_cfg.aimTracer) {
        const ImU32 tracerCol = IM_COL32(255, 214, 70, 230);
        bg->AddLine(ImVec2(scrCx, scrCy), ImVec2(aimScrX, aimScrY), IM_COL32(0, 0, 0, 160), 3.0f);
        bg->AddLine(ImVec2(scrCx, scrCy), ImVec2(aimScrX, aimScrY), tracerCol, 1.6f);
        bg->AddCircle(ImVec2(aimScrX, aimScrY), 6.5f * scale, tracerCol, 16, 1.8f);
    }

    if (hasAimTarget && ExtMemReader::IsValidUserPtr(snap.playerCameraPtr)) {
        float look[2]{};
        float rec[2]{};
        if (g_mem.ReadMem(snap.playerCameraPtr + g_off.pc_lookAngles, look, sizeof(look))) {
            g_mem.ReadMem(snap.playerCameraPtr + g_off.pc_recoilAngles, rec, sizeof(rec));
            const float adx = aimX - camX;
            const float ady = aimY - camY;
            const float adz = aimZ - camZ;
            const float horiz = std::sqrt(adx * adx + adz * adz);
            if (horiz > 0.01f &&
                std::isfinite(look[0]) && std::isfinite(look[1]) &&
                std::fabs(look[0]) <= 180.0f && std::fabs(look[1]) <= 720.0f) {
                constexpr float kRadToDeg = 57.2957795f;
                const float recPitch = std::isfinite(rec[0]) ? rec[0] : 0.0f;
                const float recYaw   = std::isfinite(rec[1]) ? rec[1] : 0.0f;

                const float targetPitch = -std::atan2(ady, horiz) * kRadToDeg - recPitch;
                const float targetYaw   =  std::atan2(adx, adz)   * kRadToDeg - recYaw;

                float dYaw = targetYaw - look[1];
                while (dYaw > 180.0f) dYaw -= 360.0f;
                while (dYaw < -180.0f) dYaw += 360.0f;
                const float dPitch = targetPitch - look[0];

                const float smooth = std::max(1.0f, g_cfg.aimSmooth);
                float outAngles[2] = {
                    look[0] + dPitch / smooth,
                    look[1] + dYaw / smooth,
                };
                if (std::isfinite(outAngles[0]) && std::isfinite(outAngles[1])) {
                    g_mem.WriteMem(snap.playerCameraPtr + g_off.pc_lookAngles, outAngles, sizeof(outAngles));
                }
            }
        }
    }
}

static void DrawExternalMenu(int sw, int sh, float uiScale) {
    const ImVec2 pillSize(92.0f * uiScale, 30.0f * uiScale);
    g_ui.pillPos.x = std::clamp(g_ui.pillPos.x, 8.0f, std::max(8.0f, static_cast<float>(sw) - pillSize.x - 8.0f));
    g_ui.pillPos.y = std::clamp(g_ui.pillPos.y, 8.0f, std::max(8.0f, static_cast<float>(sh) - pillSize.y - 8.0f));

    ImGui::SetNextWindowPos(g_ui.pillPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(pillSize, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, pillSize.y * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.10f, 0.14f, 0.88f));
    ImGui::Begin("##ExtPill", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoMove);

    if (ImGui::InvisibleButton("##ExtPillBtn", pillSize)) {
        if (!g_ui.pillDragging) g_ui.menuOpen = !g_ui.menuOpen;
        g_ui.pillDragging = false;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 5.0f)) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        g_ui.pillPos.x += d.x;
        g_ui.pillPos.y += d.y;
        g_ui.pillDragging = true;
    }
    if (ImGui::IsMouseReleased(0) && !ImGui::IsItemActive()) {
        g_ui.pillDragging = false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 p1(p0.x + pillSize.x, p0.y + pillSize.y);
    const ImU32 border = g_ui.menuOpen ? IM_COL32(99, 176, 250, 230) : IM_COL32(255, 255, 255, 50);
    dl->AddRect(p0, p1, border, pillSize.y * 0.5f, 0, 1.5f);
    const char* txt = "MWR EXT";
    const ImVec2 tSz = ImGui::CalcTextSize(txt);
    dl->AddText(ImVec2(p0.x + (pillSize.x - tSz.x) * 0.5f, p0.y + (pillSize.y - tSz.y) * 0.5f),
                g_ui.menuOpen ? IM_COL32(99, 176, 250, 255) : IM_COL32(236, 240, 246, 245), txt);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    if (!g_ui.menuOpen) return;

    const float winW = std::min(380.0f * uiScale, static_cast<float>(sw) * 0.88f);
    const float winH = std::min(340.0f * uiScale, static_cast<float>(sh) * 0.84f);
    ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2((sw - winW) * 0.5f, (sh - winH) * 0.5f), ImGuiCond_FirstUseEver);
    ImGui::Begin("MWR External", &g_ui.menuOpen,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

    if (ImGui::Button("Combat", ImVec2(98.0f * uiScale, 28.0f * uiScale))) g_ui.activeTab = 0;
    ImGui::SameLine();
    if (ImGui::Button("Visuals", ImVec2(98.0f * uiScale, 28.0f * uiScale))) g_ui.activeTab = 1;
    ImGui::SameLine();
    if (ImGui::Button("System", ImVec2(98.0f * uiScale, 28.0f * uiScale))) g_ui.activeTab = 2;
    ImGui::Separator();

    if (g_ui.activeTab == 0) {
        ImGui::Checkbox("Aimbot (LookAngles)", &g_cfg.aimbot);
        ImGui::SliderFloat("FOV radius (px)", &g_cfg.aimFovPx, 40.0f, 500.0f, "%.0f");
        ImGui::SliderFloat("Smoothing", &g_cfg.aimSmooth, 1.0f, 12.0f, "%.1f");
        const char* prioLabels[] = {"Near FOV", "Closest", "Low HP"};
        int pIdx = std::clamp(g_cfg.aimPriority, 0, 2);
        if (ImGui::Button((std::string("Priority: ") + prioLabels[pIdx]).c_str(),
                          ImVec2(170.0f * uiScale, 26.0f * uiScale))) {
            g_cfg.aimPriority = (pIdx + 1) % 3;
        }
        ImGui::SameLine();
        const char* boneLabels[] = {"Head", "Chest", "Body"};
        int bIdx = std::clamp(g_cfg.aimBone, 0, 2);
        if (ImGui::Button((std::string("Bone: ") + boneLabels[bIdx]).c_str(),
                          ImVec2(140.0f * uiScale, 26.0f * uiScale))) {
            g_cfg.aimBone = (bIdx + 1) % 3;
        }
        ImGui::Checkbox("Visibility Check", &g_cfg.aimVisCheck);
        ImGui::Checkbox("Target Tracer", &g_cfg.aimTracer);
        ImGui::Checkbox("Draw FOV Circle", &g_cfg.drawFov);
        ImGui::Checkbox("Hide Teammates (Aim)", &g_cfg.aimTeamCheck);
    } else if (g_ui.activeTab == 1) {
        ImGui::Checkbox("Enable Player ESP", &g_cfg.espEnabled);
        ImGui::Checkbox("2D Bounding Box", &g_cfg.espBox);
        ImGui::Checkbox("Snaplines", &g_cfg.espLine);
        ImGui::Checkbox("Health Bar", &g_cfg.espHp);
        ImGui::Checkbox("Name & Distance", &g_cfg.espInfo);
        ImGui::Checkbox("Hide Teammates", &g_cfg.espTeamCheck);
        ImGui::Checkbox("Visibility Check", &g_cfg.espVisCheck);
        ImGui::SliderFloat("Max Dist (m)", &g_cfg.espMaxDist, 30.0f, 500.0f, "%.0f");
    } else {
        ImGui::Text("Render: %.0f FPS | %dx%d", ImGui::GetIO().Framerate, sw, sh);
        ImGui::Text("Target PID: %d (%s)", static_cast<int>(g_mem.Pid()),
                    g_mem.IsWriteEnabled() ? "Aim RW" : "Read-Only");
        ImGui::Text("NativeCam: 0x%llx (VP+0x%x)",
                    static_cast<unsigned long long>(g_tele.nativeCamAddr), g_offNativeCamVp);
        ImGui::Text("PlayerCam: 0x%llx (look+0x%x)",
                    static_cast<unsigned long long>(g_esp.playerCameraPtr), g_off.pc_lookAngles);
        ImGui::Text("Players: %d | Enemies: %d", g_tele.playerCount, g_tele.enemyCount);
        ImGui::Text("HP: %.0f / %.0f | Team: %d", g_tele.localHp, g_tele.localMaxHp, g_tele.localTeamId);
        ImGui::Text("Status: %s", g_tele.statusText[0] ? g_tele.statusText : "Idle");
        ImGui::Separator();
        if (ImGui::Button("Rescan IL2CPP", ImVec2(150.0f * uiScale, 32.0f * uiScale))) {
            g_ui.reqRescan = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Exit Overlay", ImVec2(130.0f * uiScale, 32.0f * uiScale))) {
            g_quit.store(true);
        }
    }
    ImGui::End();
}

} // namespace

int main(int argc, char** argv) {
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);

    const char* targetArg = (argc > 1) ? argv[1] : nullptr;
    auto disp = android::ANativeWindowCreator::GetDisplayInfo();
    if (disp.width <= 0 || disp.height <= 0) {
        std::printf("[!] Failed to query SurfaceFlinger display info.\n");
        return 1;
    }

    const int maxDim = std::max(disp.width, disp.height);
    ANativeWindow* win = android::ANativeWindowCreator::Create("MwrExtOverlay", maxDim, maxDim, false, false);
    if (!win) {
        std::printf("[!] Failed to create external SurfaceControl window.\n");
        return 1;
    }
    ANativeWindow_acquire(win);

    EGLDisplay eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDisplay == EGL_NO_DISPLAY || eglInitialize(eglDisplay, nullptr, nullptr) != EGL_TRUE) {
        std::printf("[!] EGL initialization failed.\n");
        return 1;
    }

    const EGLint cfgAttribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE
    };
    EGLConfig cfg = nullptr;
    EGLint numCfg = 0;
    if (eglChooseConfig(eglDisplay, cfgAttribs, &cfg, 1, &numCfg) != EGL_TRUE || numCfg <= 0) {
        std::printf("[!] eglChooseConfig failed.\n");
        return 1;
    }

    EGLint fmt = 0;
    eglGetConfigAttrib(eglDisplay, cfg, EGL_NATIVE_VISUAL_ID, &fmt);
    ANativeWindow_setBuffersGeometry(win, 0, 0, fmt);

    const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(eglDisplay, cfg, EGL_NO_CONTEXT, ctxAttribs);
    EGLSurface surf = eglCreateWindowSurface(eglDisplay, cfg, win, nullptr);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || eglMakeCurrent(eglDisplay, surf, surf, ctx) != EGL_TRUE) {
        std::printf("[!] EGL context/surface activation failed.\n");
        return 1;
    }
    eglSwapInterval(eglDisplay, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;

    const float uiScale = std::clamp(static_cast<float>(std::min(disp.width, disp.height)) / 450.0f, 1.3f, 2.4f);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 10.0f;
    ImGui::GetStyle().FrameRounding  = 6.0f;
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.10f, 0.14f, 0.86f);
    ImGui::GetStyle().ScaleAllSizes(uiScale);
    io.FontGlobalScale = uiScale * 0.9f;

    ImGui_ImplOpenGL3_Init("#version 300 es");

    if (!ExtTouch_Init(disp.width, disp.height, disp.orientation)) {
        std::printf("[!] Warning: /dev/input touch reader failed to open (overlay display-only).\n");
    }

    pthread_t readerTh{};
    pthread_create(&readerTh, nullptr, ReaderThread, const_cast<char*>(targetArg));

    std::printf("[+] MWR External Read-Only Overlay active (%dx%d, maxDim=%d).\n", disp.width, disp.height, maxDim);
    uint32_t lastOri = disp.orientation;
    int lastW = disp.width, lastH = disp.height;
    auto lastFrame = std::chrono::steady_clock::now();

    while (!g_quit.load()) {
        disp = android::ANativeWindowCreator::GetDisplayInfo();
        if (disp.width > 0 && disp.height > 0 &&
            (disp.orientation != lastOri || disp.width != lastW || disp.height != lastH)) {
            lastOri = disp.orientation;
            lastW = disp.width;
            lastH = disp.height;
            ExtTouch_UpdateScreen(lastW, lastH, lastOri);
        }

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(now - lastFrame).count(), 0.001f, 0.1f);
        lastFrame = now;

        io.DisplaySize = ImVec2(static_cast<float>(maxDim), static_cast<float>(maxDim));
        io.DeltaTime = dt;
        ExtTouch_FeedImGui();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        DrawExternalEspAndAim(lastW, lastH);
        DrawExternalMenu(lastW, lastH, uiScale);

        ImGui::Render();
        glViewport(0, 0, maxDim, maxDim);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(eglDisplay, surf);
    }

    pthread_join(readerTh, nullptr);
    ExtTouch_Close();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(eglDisplay, surf);
    eglDestroyContext(eglDisplay, ctx);
    eglTerminate(eglDisplay);
    ANativeWindow_release(win);
    android::ANativeWindowCreator::Destroy(win);
    return 0;
}

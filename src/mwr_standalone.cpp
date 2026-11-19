#include <EGL/egl.h>
#include <android/input.h>
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <jni.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mwr_shared.h"

#define SLOGI(...) __android_log_print(ANDROID_LOG_INFO, "MwrStandalone", __VA_ARGS__)
#define SLOGE(...) __android_log_print(ANDROID_LOG_ERROR, "MwrStandalone", __VA_ARGS__)

namespace {

// ============================================================================
// Safe Read/Write Helpers
// ============================================================================
template <typename T>
inline T SafeReadVal(uint64_t addr, T fallback = T{}) {
    T v{};
    return MwrSafeRead<T>(addr, v) ? v : fallback;
}

inline uint64_t SafeReadPtr(uint64_t addr) {
    uint64_t p = 0;
    if (!MwrSafeRead<uint64_t>(addr, p)) return 0;
    return MwrIsValidUserPtr(p) ? p : 0;
}

inline bool SafeWriteMem(uint64_t addr, const void* src, size_t len) {
    uint8_t probe = 0;
    if (!MwrSafeReadMem(addr, &probe, 1)) return false;
    if (len > 1 && !MwrSafeReadMem(addr + len - 1, &probe, 1)) return false;
    std::memcpy(reinterpret_cast<void*>(addr), src, len);
    return true;
}

template <typename T>
inline bool SafeWriteVal(uint64_t addr, const T& v) {
    return SafeWriteMem(addr, &v, sizeof(T));
}

// ============================================================================
// PLT / GOT Hook Engine (zero .text patches, clean restore on Unload)
// ============================================================================
struct GotPatch {
    void** slot;
    void*  origVal;
    int    origProt;
};

static std::vector<GotPatch> g_gotPatches;

static int QueryPageProt(uintptr_t addr) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return PROT_READ | PROT_WRITE;
    char line[512];
    int prot = PROT_READ | PROT_WRITE;
    while (fgets(line, sizeof(line), f)) {
        uintptr_t start = 0, end = 0;
        char perms[8] = {};
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) == 3) {
            if (addr >= start && addr < end) {
                prot = 0;
                if (perms[0] == 'r') prot |= PROT_READ;
                if (perms[1] == 'w') prot |= PROT_WRITE;
                if (perms[2] == 'x') prot |= PROT_EXEC;
                break;
            }
        }
    }
    fclose(f);
    return prot;
}

static bool PatchGotInPhdr(struct dl_phdr_info* info, const char* elfFilter,
                           const char* symName, void* replacement, void** outOrig) {
    const char* name = info->dlpi_name ? info->dlpi_name : "";
    if (strstr(name, "libmwr_imgui") != nullptr || strstr(name, "[vdso]") != nullptr) return false;
    if (elfFilter && strstr(name, elfFilter) == nullptr) return false;

    const ElfW(Dyn)* dyn = nullptr;
    uintptr_t minVaddr = UINTPTR_MAX, maxVaddr = 0;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type == PT_LOAD) {
            if (ph.p_vaddr < minVaddr) minVaddr = ph.p_vaddr;
            if (ph.p_vaddr + ph.p_memsz > maxVaddr) maxVaddr = ph.p_vaddr + ph.p_memsz;
        } else if (ph.p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + ph.p_vaddr);
        }
    }
    if (!dyn || minVaddr == UINTPTR_MAX) return false;

    const uintptr_t minLoaded = info->dlpi_addr + minVaddr;
    const uintptr_t maxLoaded = info->dlpi_addr + maxVaddr;
    auto ResolveDynPtr = [&](uintptr_t raw) -> uintptr_t {
        if (raw == 0) return 0;
        if (raw >= minLoaded && raw < maxLoaded) return raw;
        return info->dlpi_addr + raw;
    };

    const ElfW(Sym)* symtab = nullptr;
    const char* strtab = nullptr;
    const ElfW(Rela)* jmprel = nullptr;
    size_t jmprelSz = 0;
    ElfW(Xword) pltrelType = DT_RELA;

    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
        switch (d->d_tag) {
            case DT_SYMTAB:   symtab = reinterpret_cast<const ElfW(Sym)*>(ResolveDynPtr(d->d_un.d_ptr)); break;
            case DT_STRTAB:   strtab = reinterpret_cast<const char*>(ResolveDynPtr(d->d_un.d_ptr)); break;
            case DT_JMPREL:   jmprel = reinterpret_cast<const ElfW(Rela)*>(ResolveDynPtr(d->d_un.d_ptr)); break;
            case DT_PLTRELSZ: jmprelSz = static_cast<size_t>(d->d_un.d_val); break;
            case DT_PLTREL:   pltrelType = d->d_un.d_val; break;
            default: break;
        }
    }
    if (!symtab || !strtab || !jmprel || jmprelSz == 0 || pltrelType != DT_RELA) return false;

    const size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const size_t count = jmprelSz / sizeof(ElfW(Rela));
    bool patchedAny = false;

    for (size_t i = 0; i < count; ++i) {
        const uint32_t symIdx = ELF64_R_SYM(jmprel[i].r_info);
        const char* sname = strtab + symtab[symIdx].st_name;
        if (std::strcmp(sname, symName) != 0) continue;

        auto** slot = reinterpret_cast<void**>(info->dlpi_addr + jmprel[i].r_offset);
        for (const auto& existing : g_gotPatches) {
            if (existing.slot == slot) return false;
        }

        const uintptr_t pageStart = reinterpret_cast<uintptr_t>(slot) & ~(pageSize - 1);
        const int origProt = QueryPageProt(reinterpret_cast<uintptr_t>(slot));
        if ((origProt & PROT_WRITE) == 0) {
            if (mprotect(reinterpret_cast<void*>(pageStart), pageSize, PROT_READ | PROT_WRITE) != 0) {
                continue;
            }
        }

        void* orig = *slot;
        if (outOrig && !*outOrig && orig != replacement) {
            *outOrig = orig;
        }
        *slot = replacement;
        if ((origProt & PROT_WRITE) == 0) {
            mprotect(reinterpret_cast<void*>(pageStart), pageSize, origProt);
        }
        g_gotPatches.push_back({slot, orig, origProt});
        patchedAny = true;
        SLOGI("PLT patched %s in %s (slot=%p orig=%p)", symName, name[0] ? name : "<self>", slot, orig);
    }
    return patchedAny;
}

static int InstallPltHook(const char* elfFilter, const char* symName, void* replacement, void** outOrig) {
    struct Ctx {
        const char* filter;
        const char* sym;
        void* repl;
        void** orig;
        int hits;
    } ctx{elfFilter, symName, replacement, outOrig, 0};

    dl_iterate_phdr(
        [](struct dl_phdr_info* info, size_t, void* data) -> int {
            auto* c = static_cast<Ctx*>(data);
            if (PatchGotInPhdr(info, c->filter, c->sym, c->repl, c->orig)) {
                ++c->hits;
            }
            return 0;
        },
        &ctx);
    return ctx.hits;
}

static void RestorePltHooks() {
    const size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    for (auto it = g_gotPatches.rbegin(); it != g_gotPatches.rend(); ++it) {
        const uintptr_t pageStart = reinterpret_cast<uintptr_t>(it->slot) & ~(pageSize - 1);
        if ((it->origProt & PROT_WRITE) == 0) {
            mprotect(reinterpret_cast<void*>(pageStart), pageSize, PROT_READ | PROT_WRITE);
        }
        *(it->slot) = it->origVal;
        if ((it->origProt & PROT_WRITE) == 0) {
            mprotect(reinterpret_cast<void*>(pageStart), pageSize, it->origProt);
        }
    }
    g_gotPatches.clear();
}

// ============================================================================
// eglSwapBuffers & InputConsumer::consume Hooks
// ============================================================================
using FnEglSwapBuffers = EGLBoolean (*)(EGLDisplay, EGLSurface);
using FnInputConsume = int32_t (*)(void*, void*, bool, int64_t, uint32_t*, void**);

static FnEglSwapBuffers g_origEglSwap = nullptr;
static FnInputConsume   g_origInputConsume = nullptr;
static std::atomic<bool> g_detaching{false};
static std::atomic<int>  g_decorW{0};
static std::atomic<int>  g_decorH{0};

static EGLBoolean HookEglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!g_detaching.load()) {
        imgui_on_swap(dpy, surf);
    }
    return g_origEglSwap ? g_origEglSwap(dpy, surf) : EGL_TRUE;
}

static inline void GetTouchScale(float& sx, float& sy) {
    const int vw = g_decorW.load();
    const int vh = g_decorH.load();
    if (vw > 0 && vh > 0) {
        int sw = 0, sh = 0;
        imgui_get_surface_size(&sw, &sh);
        if (sw > 0 && sh > 0) {
            sx = static_cast<float>(sw) / static_cast<float>(vw);
            sy = static_cast<float>(sh) / static_cast<float>(vh);
            return;
        }
    }
    sx = 1.0f;
    sy = 1.0f;
}

static int32_t HookInputConsume(void* self, void* factory, bool consumeBatched,
                                int64_t frameTime, uint32_t* outSeq, void** outEvent) {
    const int32_t rc = g_origInputConsume
        ? g_origInputConsume(self, factory, consumeBatched, frameTime, outSeq, outEvent)
        : 0;

    if (rc == 0 && outEvent && *outEvent && !g_detaching.load()) {
        const auto* ev = reinterpret_cast<const AInputEvent*>(*outEvent);
        if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION) {
            const int32_t fullAction = AMotionEvent_getAction(ev);
            const int32_t actMasked = fullAction & AMOTION_EVENT_ACTION_MASK;
            float sx = 1.0f, sy = 1.0f;
            GetTouchScale(sx, sy);

            bool consumed = false;
            if (actMasked == AMOTION_EVENT_ACTION_MOVE) {
                const size_t cnt = AMotionEvent_getPointerCount(ev);
                size_t consumedCnt = 0;
                for (size_t i = 0; i < cnt; ++i) {
                    const int32_t pid = AMotionEvent_getPointerId(ev, i);
                    const float x = AMotionEvent_getX(ev, i) * sx;
                    const float y = AMotionEvent_getY(ev, i) * sy;
                    if (imgui_on_touch(2, pid, x, y) != 0) ++consumedCnt;
                }
                consumed = (cnt > 0 && consumedCnt == cnt);
            } else {
                const size_t idx = static_cast<size_t>(
                    (fullAction & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                    AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
                const int32_t pid = AMotionEvent_getPointerId(ev, idx);
                const float x = AMotionEvent_getX(ev, idx) * sx;
                const float y = AMotionEvent_getY(ev, idx) * sy;
                if (imgui_on_touch(actMasked, pid, x, y) != 0) {
                    if (AMotionEvent_getPointerCount(ev) <= 1 ||
                        actMasked == AMOTION_EVENT_ACTION_DOWN ||
                        actMasked == AMOTION_EVENT_ACTION_POINTER_DOWN) {
                        consumed = true;
                    }
                }
            }

            if (consumed) {
                // Verify mAction offset (+0x38 in Android MotionEvent) matches AMotionEvent_getAction before cancelling
                auto* actPtr = reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(*outEvent) + 0x38);
                if (*actPtr == fullAction) {
                    *actPtr = AMOTION_EVENT_ACTION_CANCEL;
                }
            }
        }
    }
    return rc;
}

static void RefreshDecorSize(JavaVM* vm, bool& attached) {
    if (!vm) return;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }
    if (!env) return;

    jclass clsUnity = env->FindClass("com/unity3d/player/UnityPlayer");
    if (!clsUnity || env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
    }
    jfieldID fidAct = env->GetStaticFieldID(clsUnity, "currentActivity", "Landroid/app/Activity;");
    if (!fidAct || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(clsUnity);
        return;
    }
    jobject act = env->GetStaticObjectField(clsUnity, fidAct);
    env->DeleteLocalRef(clsUnity);
    if (!act || env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
    }

    jclass clsAct = env->GetObjectClass(act);
    jmethodID midWin = env->GetMethodID(clsAct, "getWindow", "()Landroid/view/Window;");
    jobject win = midWin ? env->CallObjectMethod(act, midWin) : nullptr;
    env->DeleteLocalRef(clsAct);
    env->DeleteLocalRef(act);
    if (!win || env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
    }

    jclass clsWin = env->GetObjectClass(win);
    jmethodID midDecor = env->GetMethodID(clsWin, "getDecorView", "()Landroid/view/View;");
    jobject decor = midDecor ? env->CallObjectMethod(win, midDecor) : nullptr;
    env->DeleteLocalRef(clsWin);
    env->DeleteLocalRef(win);
    if (!decor || env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
    }

    jclass clsView = env->GetObjectClass(decor);
    jmethodID midW = env->GetMethodID(clsView, "getWidth", "()I");
    jmethodID midH = env->GetMethodID(clsView, "getHeight", "()I");
    if (midW && midH) {
        int vw = env->CallIntMethod(decor, midW);
        int vh = env->CallIntMethod(decor, midH);
        if (!env->ExceptionCheck() && vw > 0 && vh > 0) {
            g_decorW.store(vw);
            g_decorH.store(vh);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(clsView);
    env->DeleteLocalRef(decor);
}

// ============================================================================
// IL2CPP Reflection & Field Offsets (dynamic resolve + v24 defaults)
// ============================================================================
struct Offsets {
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
    uint32_t lpm_debugSpeedMulti = 0x10;
    uint32_t lpm_localPlayer = 0x18;
    uint32_t lpm_bindNetId = 0x20;

    uint32_t cm_instance = 0x00;
    uint32_t cm_camera = 0x18;
    uint32_t cm_cameraTf = 0x28;

    uint32_t ac_sceneWall = 0x00;
    uint32_t ac_teleport = 0x01;
    uint32_t ac_flyCheck = 0x08;
    uint32_t ac_block2Rep = 0x09;
    uint32_t ac_block2Reply = 0x0a;
    uint32_t ac_block4Rep = 0x0b;

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

    uint32_t pl_equippedItemVal = 0x1d8;
    uint32_t val_currentValue = 0x20;
    uint32_t pl_movement = 0x4d8;
    uint32_t pl_camera = 0x4e8;

    uint32_t pc_physHandler = 0x28;
    uint32_t pc_lookAngles = 0x50;
    uint32_t pc_recoilAngles = 0x58;
    uint32_t pc_recoilEnabled = 0x60;
    uint32_t cph_recoilSpring = 0x68;

    uint32_t pmov_gravityMod = 0xd8;
    uint32_t gm_gravityMul = 0x14;
    uint32_t gm_fallSpeedMul = 0x1c;

    uint32_t eq_nextUseTime = 0xd4;
    uint32_t pw_curAccurate = 0x110;
    uint32_t pw_shootAccurate = 0x114;
    uint32_t pw_accurateRatio = 0x11c;
    uint32_t pw_mPW = 0x140;
    uint32_t pwi_shooting = 0x38;
    uint32_t sh_fireDur = 0x14;
    uint32_t sh_fireDurAuto = 0x18;
    uint32_t pw_fireDurTimer = 0x174;
};

struct Il2CppApi {
    void* (*domain_get)() = nullptr;
    const void** (*domain_get_assemblies)(void* domain, size_t* size) = nullptr;
    void* (*assembly_get_image)(const void* assembly) = nullptr;
    size_t (*image_get_class_count)(void* image) = nullptr;
    void* (*image_get_class)(void* image, size_t index) = nullptr;
    const char* (*class_get_name)(void* klass) = nullptr;
    void* (*class_from_name)(void* image, const char* ns, const char* name) = nullptr;
    void* (*class_get_static_field_data)(void* klass) = nullptr;
    void* (*class_get_field_from_name)(void* klass, const char* name) = nullptr;
    size_t (*field_get_offset)(void* field) = nullptr;
    bool loaded = false;

    bool Init() {
        if (loaded) return true;
        void* h = dlopen("libil2cpp.so", RTLD_NOLOAD);
        if (!h) return false;
        domain_get = reinterpret_cast<decltype(domain_get)>(dlsym(h, "il2cpp_domain_get"));
        domain_get_assemblies = reinterpret_cast<decltype(domain_get_assemblies)>(dlsym(h, "il2cpp_domain_get_assemblies"));
        assembly_get_image = reinterpret_cast<decltype(assembly_get_image)>(dlsym(h, "il2cpp_assembly_get_image"));
        image_get_class_count = reinterpret_cast<decltype(image_get_class_count)>(dlsym(h, "il2cpp_image_get_class_count"));
        image_get_class = reinterpret_cast<decltype(image_get_class)>(dlsym(h, "il2cpp_image_get_class"));
        class_get_name = reinterpret_cast<decltype(class_get_name)>(dlsym(h, "il2cpp_class_get_name"));
        class_from_name = reinterpret_cast<decltype(class_from_name)>(dlsym(h, "il2cpp_class_from_name"));
        class_get_static_field_data = reinterpret_cast<decltype(class_get_static_field_data)>(dlsym(h, "il2cpp_class_get_static_field_data"));
        class_get_field_from_name = reinterpret_cast<decltype(class_get_field_from_name)>(dlsym(h, "il2cpp_class_get_field_from_name"));
        field_get_offset = reinterpret_cast<decltype(field_get_offset)>(dlsym(h, "il2cpp_field_get_offset"));
        loaded = (domain_get && domain_get_assemblies && assembly_get_image &&
                  image_get_class_count && image_get_class && class_get_name);
        return loaded;
    }

    uint64_t GetStaticData(void* klass, const Offsets& off) const {
        if (!klass) return 0;
        if (class_get_static_field_data) {
            void* st = class_get_static_field_data(klass);
            uint64_t addr = reinterpret_cast<uint64_t>(st);
            if (MwrIsValidUserPtr(addr)) return addr;
        }
        return SafeReadPtr(reinterpret_cast<uint64_t>(klass) + off.klass_static);
    }

    void ResolveField(void* klass, const char* name, uint32_t& target, bool subtractHeader = false) const {
        if (!klass || !class_get_field_from_name || !field_get_offset) return;
        void* f = class_get_field_from_name(klass, name);
        if (!f) return;
        size_t o = field_get_offset(f);
        if (subtractHeader) {
            target = static_cast<uint32_t>(o);
        } else {
            target = static_cast<uint32_t>(o);
        }
    }
};

struct Classes {
    void* PlayerManager = nullptr;
    void* LocalPlayerManager = nullptr;
    void* CameraManager = nullptr;
    void* AntiCheatMgr = nullptr;
    void* NetPlayerBase = nullptr;
    void* Hero = nullptr;
    void* NetPlayerData2 = nullptr;
    void* Battle = nullptr;
    void* Player = nullptr;
    void* PlayerCamera = nullptr;
    void* CameraPhysicsHandler = nullptr;
    void* PlayerMovement = nullptr;
    void* GravityModule = nullptr;
    void* ActorPlayerShareData = nullptr;
    void* ActorHeroShareData = nullptr;
    void* AvatarRendererComp = nullptr;
    void* ProjectileWeapon = nullptr;
    void* ProjectileWeaponInfo = nullptr;
    void* ShootingInfo = nullptr;
    void* UnityObject = nullptr;
};

static Offsets   g_off;
static Il2CppApi g_il2cpp;
static Classes   g_cls;
static std::unordered_set<uint64_t> g_resolvedObjKlasses;

struct PlayerCacheEntry {
    int32_t  netId = -1;
    std::string teamStr;
    std::string name;
    uint64_t natRend = 0;
    uint64_t av = 0;
};

struct OrigValues {
    bool speedCaptured = false;
    float debugSpeedMulti = 1.0f;
    bool gravCaptured = false;
    float gravityMul = 1.0f;
    float fallSpeedMul = 1.0f;
    bool recoilCaptured = false;
    uint8_t recoilEnabled = 1;
    bool acCaptured = false;
    uint8_t acBytes[6] = {1, 1, 1, 0, 0, 0};

    struct SpreadOrig {
        uint64_t ptr;
        float accRatio;
    };
    struct ShootOrig {
        uint64_t ptr;
        float fireDur;
        float fireDurAuto;
    };
    std::unordered_map<uint64_t, SpreadOrig> spreadMap;
    std::unordered_map<uint64_t, ShootOrig> shootMap;
};

static OrigValues g_orig;
static std::unordered_map<uint64_t, PlayerCacheEntry> g_playerCache;
static uint64_t g_cachedLocalHero = 0;
static std::string g_cachedLocalTeamStr;
static bool g_lastAc = false, g_lastSpeed = false, g_lastGrav = false;
static bool g_lastRecoil = false, g_lastSpread = false, g_lastRapid = false;

static constexpr uint64_t kPtrTagMask = 0x00ffffffffffffffULL;
static uint64_t g_metaBase = 0;
static uint32_t g_metaStrOff = 0;
static uint32_t g_metaStrSize = 0;
static char     g_scanDiag[64] = "Scanning IL2CPP...";

struct MapsRegion {
    uint64_t    start = 0;
    uint64_t    end = 0;
    char        perms[8]{};
    std::string path;
};

static std::vector<MapsRegion> ReadProcMaps() {
    std::vector<MapsRegion> out;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return out;
    char line[768];
    while (fgets(line, sizeof(line), f)) {
        uint64_t start = 0, end = 0;
        char perms[8]{};
        int pathIdx = 0;
        if (sscanf(line, "%lx-%lx %7s %*x %*s %*lu %n", &start, &end, perms, &pathIdx) >= 3) {
            MapsRegion r;
            r.start = start;
            r.end = end;
            std::memcpy(r.perms, perms, sizeof(r.perms));
            if (pathIdx > 0 && line[pathIdx] != '\0') {
                std::string p(line + pathIdx);
                while (!p.empty() && (p.back() == '\n' || p.back() == '\r' || p.back() == ' ')) {
                    p.pop_back();
                }
                r.path = std::move(p);
            }
            out.push_back(std::move(r));
        }
    }
    fclose(f);
    return out;
}

static bool ShouldSkipScanRegion(const MapsRegion& r) {
    if (r.end <= r.start) return true;
    if (r.path.empty()) return false;
    const char* p = r.path.c_str();
    return std::strstr(p, "libmwr_imgui") != nullptr ||
           std::strstr(p, "[vdso]") != nullptr ||
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
    const int32_t ver = SafeReadVal<int32_t>(candBase + 4, 0);
    if (ver < 21 || ver > 31) return false;
    const uint32_t strOff = SafeReadVal<uint32_t>(candBase + 24, 0);
    const uint32_t strSize = SafeReadVal<uint32_t>(candBase + 28, 0);
    if (strOff < 0x1000U || strOff > 0x800000U) return false;
    if (strSize < 0x40000U || strSize > 0x1000000U) return false;
    if (candBase + static_cast<uint64_t>(strOff) + static_cast<uint64_t>(strSize) > regionEnd) return false;

    char sig[9]{};
    if (!MwrSafeReadMem(candBase + strOff, sig, sizeof(sig))) return false;
    if (std::memcmp(sig, "mscorlib", 9) != 0) return false;

    outStrOff = strOff;
    outStrSize = strSize;
    return true;
}

static bool FindMetadataBaseFromMaps(const std::vector<MapsRegion>& regions) {
    if (g_metaBase && MwrIsPageReadable(g_metaBase & ~4095ULL)) return true;
    g_metaBase = 0;

    // Pass 1: standard decrypted metadata mmap size (~0x12c8000, including rw-s /dev/zero)
    for (const auto& r : regions) {
        if (r.perms[0] != 'r' || (r.perms[3] != 'p' && r.perms[3] != 's')) continue;
        if (ShouldSkipScanRegion(r) || !MwrIsValidAddrRange(r.start)) continue;
        const uint64_t sz = r.end - r.start;
        if (sz >= 0x1000000ULL && sz <= 0x1600000ULL) {
            if (ValidateMetadataCandidate(r.start, r.end, g_metaStrOff, g_metaStrSize)) {
                g_metaBase = r.start;
                SLOGI("Metadata (pass1) @ 0x%lx strOff=0x%x strSize=0x%x",
                      (unsigned long)g_metaBase, g_metaStrOff, g_metaStrSize);
                return true;
            }
        }
    }

    // Pass 2: fallback if Scudo merged VMAs or added an allocator header
    for (const auto& r : regions) {
        if (r.perms[0] != 'r' || (r.perms[3] != 'p' && r.perms[3] != 's')) continue;
        if (ShouldSkipScanRegion(r) || !MwrIsValidAddrRange(r.start)) continue;
        const uint64_t sz = r.end - r.start;
        if (sz < 0x1000000ULL) continue;
        for (uint64_t page = r.start; page + 0x1000000ULL <= r.end; page += 4096ULL) {
            if (!MwrIsPageReadable(page)) continue;
            const size_t lim = std::min<size_t>(4096, static_cast<size_t>(r.end - page));
            if (lim < 12) continue;
            const auto* p = reinterpret_cast<const uint32_t*>(page);
            const size_t n = lim / 4;
            for (size_t i = 1; i + 1 < n; ++i) {
                if (p[i] == 24U && p[i + 1] == 256U) {
                    const uint64_t cand = page + i * 4ULL - 4ULL;
                    if (ValidateMetadataCandidate(cand, r.end, g_metaStrOff, g_metaStrSize)) {
                        g_metaBase = cand;
                        SLOGI("Metadata (pass2) @ 0x%lx strOff=0x%x strSize=0x%x",
                              (unsigned long)g_metaBase, g_metaStrOff, g_metaStrSize);
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static bool FindStringTableFallback(const std::vector<MapsRegion>& regions,
                                    uint64_t& outStrBase, uint32_t& outStrSize) {
    static const char kPm[] = "\0PlayerManager\0";
    static const char kCm[] = "\0CameraManager\0";
    for (const auto& r : regions) {
        if (r.perms[0] != 'r' || (r.perms[3] != 'p' && r.perms[3] != 's')) continue;
        if (ShouldSkipScanRegion(r) || !MwrIsValidAddrRange(r.start)) continue;
        const uint64_t sz = r.end - r.start;
        if (sz < 0x200000ULL || sz > 0x3000000ULL) continue;

        bool hasPm = false, hasCm = false;
        for (uint64_t page = r.start; page < r.end; page += 4096ULL) {
            if (!MwrIsPageReadable(page)) continue;
            const size_t lim = std::min<size_t>(4096, static_cast<size_t>(r.end - page));
            const auto* buf = reinterpret_cast<const uint8_t*>(page);
            if (!hasPm && lim >= sizeof(kPm) - 1 &&
                memmem(buf, lim, kPm, sizeof(kPm) - 1) != nullptr) {
                hasPm = true;
            }
            if (!hasCm && lim >= sizeof(kCm) - 1 &&
                memmem(buf, lim, kCm, sizeof(kCm) - 1) != nullptr) {
                hasCm = true;
            }
            if (hasPm && hasCm) {
                outStrBase = r.start;
                outStrSize = static_cast<uint32_t>(std::min<uint64_t>(sz, 0x2000000ULL));
                SLOGI("Metadata string fallback @ 0x%lx size=0x%x", (unsigned long)outStrBase, outStrSize);
                return true;
            }
        }
    }
    return false;
}

static bool ResolveFieldInSingleClass(uint64_t klass, const char* fieldName, uint32_t& outOff) {
    if (!MwrIsValidUserPtr(klass) || !fieldName) return false;
    const uint64_t fieldsPtr = SafeReadPtr(klass + g_off.klass_fields);
    if (!MwrIsValidUserPtr(fieldsPtr)) return false;

    const uint64_t klassU = klass & kPtrTagMask;
    for (int i = 0; i < 240; ++i) {
        const uint64_t fi = fieldsPtr + static_cast<uint64_t>(i) * 32ULL;
        const uint64_t fParent = SafeReadPtr(fi + 0x10);
        if ((fParent & kPtrTagMask) != klassU) break;

        const uint64_t namePtr = SafeReadPtr(fi + 0x00);
        if (!MwrIsValidAddrRange(namePtr)) continue;

        char buf[64]{};
        if (!MwrSafeReadMem(namePtr, buf, sizeof(buf) - 1)) continue;
        buf[sizeof(buf) - 1] = '\0';
        if (std::strcmp(buf, fieldName) == 0) {
            const int32_t off = SafeReadVal<int32_t>(fi + 0x18, -1);
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
    for (int d = 0; d < maxDepth && MwrIsValidUserPtr(cur); ++d) {
        if (ResolveFieldInSingleClass(cur, fieldName, outOff)) return true;
        cur = SafeReadPtr(cur + g_off.klass_parent);
    }
    return false;
}

static bool ResolveFieldAlias(uint64_t klass, const char* const* names, size_t nNames, uint32_t& outOff, int maxDepth = 5) {
    for (size_t i = 0; i < nNames; ++i) {
        if (ResolveFieldFromChain(klass, names[i], outOff, maxDepth)) return true;
    }
    return false;
}

static uint32_t DetectFieldsOffset(uint64_t klass) {
    if (!MwrIsValidUserPtr(klass)) return g_off.klass_fields;
    const uint64_t kU = klass & kPtrTagMask;
    const uint32_t candidates[] = {g_off.klass_fields, 0x80, 0x78, 0x88, 0x70, 0x90, 0x98, 0xa0};
    for (uint32_t cand : candidates) {
        uint64_t fld = SafeReadPtr(klass + cand);
        uint64_t fp = fld ? SafeReadPtr(fld + 0x10) : 0;
        if (fp && (fp & kPtrTagMask) == kU) {
            g_off.klass_fields = cand;
            return cand;
        }
    }
    return g_off.klass_fields;
}

static void ApplyAllClassOffsets() {
    auto K = [](void* p) { return reinterpret_cast<uint64_t>(p); };

    if (g_cls.UnityObject) {
        ResolveFieldFromChain(K(g_cls.UnityObject), "m_CachedPtr", g_off.obj_cachedPtr, 2);
    }
    if (g_cls.PlayerManager) {
        ResolveFieldFromChain(K(g_cls.PlayerManager), "LocalHero", g_off.pm_localHero, 2);
        ResolveFieldFromChain(K(g_cls.PlayerManager), "_playerDict", g_off.pm_playerDict, 2);
    }
    if (g_cls.LocalPlayerManager) {
        ResolveFieldFromChain(K(g_cls.LocalPlayerManager), "_instance", g_off.lpm_instance, 2);
        ResolveFieldFromChain(K(g_cls.LocalPlayerManager), "DebugSpeedMulti", g_off.lpm_debugSpeedMulti, 2);
        ResolveFieldFromChain(K(g_cls.LocalPlayerManager), "mLocalPlayer", g_off.lpm_localPlayer, 2);
        ResolveFieldFromChain(K(g_cls.LocalPlayerManager), "curBindNetPlayerId", g_off.lpm_bindNetId, 2);
    }
    if (g_cls.CameraManager) {
        ResolveFieldFromChain(K(g_cls.CameraManager), "instance", g_off.cm_instance, 2);
        ResolveFieldFromChain(K(g_cls.CameraManager), "mCamera", g_off.cm_camera, 2);
        ResolveFieldFromChain(K(g_cls.CameraManager), "mCameraTransform", g_off.cm_cameraTf, 2);
    }
    if (g_cls.AntiCheatMgr) {
        const uint64_t k = K(g_cls.AntiCheatMgr);
        ResolveFieldFromChain(k, "enableSceneWallCheck", g_off.ac_sceneWall, 2);
        ResolveFieldFromChain(k, "enableValidTeleportCheck", g_off.ac_teleport, 2);
        ResolveFieldFromChain(k, "enableFlyCheckModel", g_off.ac_flyCheck, 2);
        ResolveFieldFromChain(k, "<BlockAceData2Report>k__BackingField", g_off.ac_block2Rep, 2);
        ResolveFieldFromChain(k, "<BlockAceData2Reply>k__BackingField", g_off.ac_block2Reply, 2);
        ResolveFieldFromChain(k, "<BlockAceData4Report>k__BackingField", g_off.ac_block4Rep, 2);
    }
    if (g_cls.NetPlayerBase) {
        ResolveFieldFromChain(K(g_cls.NetPlayerBase), "<mTransform>k__BackingField", g_off.np_transform, 2);
        ResolveFieldFromChain(K(g_cls.NetPlayerBase), "isDestory", g_off.np_isDestroy, 2);
    }
    if (g_cls.Hero) {
        ResolveFieldFromChain(K(g_cls.Hero), "<mShareData>k__BackingField", g_off.np_shareData, 2);
    }
    if (g_cls.ActorHeroShareData) {
        ResolveFieldFromChain(K(g_cls.ActorHeroShareData), "<netData>k__BackingField", g_off.ahs_netData, 2);
        ResolveFieldFromChain(K(g_cls.ActorHeroShareData), "_baseData", g_off.ahs_baseData, 2);
    }
    if (g_cls.ActorPlayerShareData) {
        const uint64_t k = K(g_cls.ActorPlayerShareData);
        ResolveFieldFromChain(k, "<playerTrans>k__BackingField", g_off.aps_playerTrans, 2);
        ResolveFieldFromChain(k, "<netData>k__BackingField", g_off.aps_netData, 2);
        ResolveFieldFromChain(k, "avatarController", g_off.aps_avatarCtrl, 2);
        ResolveFieldFromChain(k, "netPlayerId", g_off.aps_netPlayerId, 2);
        ResolveFieldFromChain(k, "_baseData", g_off.aps_baseData, 2);
    }
    if (g_cls.AvatarRendererComp) {
        ResolveFieldFromChain(K(g_cls.AvatarRendererComp), "RendererDict", g_off.ar_rendDict, 2);
    }
    if (g_cls.NetPlayerData2) {
        const uint64_t k = K(g_cls.NetPlayerData2);
        ResolveFieldFromChain(k, "PlayerPosition", g_off.npd_playerPos, 3);
        ResolveFieldFromChain(k, "battle", g_off.npd_battle, 3);
        ResolveFieldFromChain(k, "<TeamIdStr>k__BackingField", g_off.npd_teamIdStr, 3);
        ResolveFieldFromChain(k, "<PlayerNameStr>k__BackingField", g_off.npd_playerNameStr, 3);
    }
    if (g_cls.Battle) {
        ResolveFieldFromChain(K(g_cls.Battle), "MaxHp", g_off.bat_maxHp, 2);
        ResolveFieldFromChain(K(g_cls.Battle), "Hp", g_off.bat_hp, 2);
    }
    if (g_cls.Player) {
        const uint64_t k = K(g_cls.Player);
        ResolveFieldFromChain(k, "EquippedItem", g_off.pl_equippedItemVal, 4);
        ResolveFieldFromChain(k, "_PlayerMovement", g_off.pl_movement, 4);
        ResolveFieldFromChain(k, "_PlayerCamera", g_off.pl_camera, 4);
    }
    if (g_cls.PlayerCamera) {
        const uint64_t k = K(g_cls.PlayerCamera);
        ResolveFieldFromChain(k, "m_CameraPhysicsHandler", g_off.pc_physHandler, 3);
        ResolveFieldFromChain(k, "m_LookAngles", g_off.pc_lookAngles, 3);
        ResolveFieldFromChain(k, "m_RecoilAngles", g_off.pc_recoilAngles, 3);
        ResolveFieldFromChain(k, "m_RecoilEnabled", g_off.pc_recoilEnabled, 3);
        g_mwrCamOff.lookAngles    = g_off.pc_lookAngles;
        g_mwrCamOff.recoilAngles  = g_off.pc_recoilAngles;
        g_mwrCamOff.recoilEnabled = g_off.pc_recoilEnabled;
    }
    if (g_cls.CameraPhysicsHandler) {
        ResolveFieldFromChain(K(g_cls.CameraPhysicsHandler), "m_RotationSpring_Recoil", g_off.cph_recoilSpring, 2);
    }
    if (g_cls.PlayerMovement) {
        ResolveFieldFromChain(K(g_cls.PlayerMovement), "m_GravityModule", g_off.pmov_gravityMod, 3);
    }
    if (g_cls.GravityModule) {
        ResolveFieldFromChain(K(g_cls.GravityModule), "GravityMul", g_off.gm_gravityMul, 2);
        ResolveFieldFromChain(K(g_cls.GravityModule), "FallSpeedMul", g_off.gm_fallSpeedMul, 2);
    }
    if (g_cls.ProjectileWeapon) {
        const uint64_t k = K(g_cls.ProjectileWeapon);
        ResolveFieldFromChain(k, "m_NextTimeCanUse", g_off.eq_nextUseTime, 4);
        ResolveFieldFromChain(k, "curAccurate", g_off.pw_curAccurate, 4);
        ResolveFieldFromChain(k, "shootAccurate", g_off.pw_shootAccurate, 4);
        ResolveFieldFromChain(k, "accurateRatio", g_off.pw_accurateRatio, 4);
        ResolveFieldFromChain(k, "m_PW", g_off.pw_mPW, 4);
        ResolveFieldFromChain(k, "fireDurationCheckTimer", g_off.pw_fireDurTimer, 4);
    }
    if (g_cls.ProjectileWeaponInfo) {
        ResolveFieldFromChain(K(g_cls.ProjectileWeaponInfo), "Shooting", g_off.pwi_shooting, 2);
    }
    if (g_cls.ShootingInfo) {
        ResolveFieldFromChain(K(g_cls.ShootingInfo), "FireDuration", g_off.sh_fireDur, 2);
        ResolveFieldFromChain(K(g_cls.ShootingInfo), "FireDurationAutoFire", g_off.sh_fireDurAuto, 2);
    }
}

enum class LiveObjKind {
    Dict, List, NetVar, QueuedVal, AvatarCtrl, AvatarRend,
    Player, PlayerCam, CamPhys, RecoilSpring, Movement, Gravity,
    Gun, WeaponInfo, Shooting, ShareData, HeroShare, HeroBaseData, PlayerBaseData, NetPlayerData, Battle
};

static void EnsureLiveObjectOffsets(uint64_t obj, LiveObjKind kind) {
    if (!MwrIsValidUserPtr(obj)) return;
    const uint64_t klass = SafeReadPtr(obj);
    if (!MwrIsValidUserPtr(klass)) return;
    const uint64_t klassU = klass & kPtrTagMask;
    if (g_resolvedObjKlasses.find(klassU) != g_resolvedObjKlasses.end()) return;
    g_resolvedObjKlasses.insert(klassU);

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
        case LiveObjKind::QueuedVal: {
            const char* qAliases[] = {"m_CurrentValue", "<CurrentValue>k__BackingField", "currentValue"};
            ResolveFieldAlias(klass, qAliases, 3, g_off.val_currentValue, 3);
            break;
        }
        case LiveObjKind::AvatarCtrl: {
            const char* mrAliases[] = {"mRenderer", "_mRenderer"};
            ResolveFieldAlias(klass, mrAliases, 2, g_off.av_mRenderer, 3);
            ResolveFieldFromChain(klass, "renderIsShow", g_off.av_renderShow, 3);
            break;
        }
        case LiveObjKind::AvatarRend:
            if (!g_cls.AvatarRendererComp) g_cls.AvatarRendererComp = reinterpret_cast<void*>(klass);
            ResolveFieldFromChain(klass, "RendererDict", g_off.ar_rendDict, 2);
            break;
        case LiveObjKind::Player:
            if (!g_cls.Player) {
                g_cls.Player = reinterpret_cast<void*>(klass);
                ApplyAllClassOffsets();
            }
            break;
        case LiveObjKind::PlayerCam:
            if (!g_cls.PlayerCamera) {
                g_cls.PlayerCamera = reinterpret_cast<void*>(klass);
                ApplyAllClassOffsets();
            }
            break;
        case LiveObjKind::CamPhys:
            if (!g_cls.CameraPhysicsHandler) {
                g_cls.CameraPhysicsHandler = reinterpret_cast<void*>(klass);
                ApplyAllClassOffsets();
            }
            break;
        case LiveObjKind::RecoilSpring: {
            uint32_t sp = g_mwrCamOff.springPos, df = 0;
            if (ResolveFieldFromChain(klass, "m_Position", sp, 2)) {
                g_mwrCamOff.springPos = sp;
                if (ResolveFieldFromChain(klass, "m_DistributedForce", df, 2) && df >= sp) {
                    const uint32_t len = (df + 12U) - sp;
                    if (len >= 12U && len <= 96U) g_mwrCamOff.springClearLen = len;
                }
            }
            break;
        }
        case LiveObjKind::Movement:
            if (!g_cls.PlayerMovement) {
                g_cls.PlayerMovement = reinterpret_cast<void*>(klass);
                ApplyAllClassOffsets();
            }
            break;
        case LiveObjKind::Gravity:
            if (!g_cls.GravityModule) {
                g_cls.GravityModule = reinterpret_cast<void*>(klass);
                ApplyAllClassOffsets();
            }
            break;
        case LiveObjKind::Gun: {
            ResolveFieldFromChain(klass, "m_NextTimeCanUse", g_off.eq_nextUseTime, 4);
            ResolveFieldFromChain(klass, "curAccurate", g_off.pw_curAccurate, 4);
            ResolveFieldFromChain(klass, "shootAccurate", g_off.pw_shootAccurate, 4);
            ResolveFieldFromChain(klass, "accurateRatio", g_off.pw_accurateRatio, 4);
            ResolveFieldFromChain(klass, "m_PW", g_off.pw_mPW, 4);
            ResolveFieldFromChain(klass, "fireDurationCheckTimer", g_off.pw_fireDurTimer, 4);
            break;
        }
        case LiveObjKind::WeaponInfo:
            if (!g_cls.ProjectileWeaponInfo) {
                g_cls.ProjectileWeaponInfo = reinterpret_cast<void*>(klass);
                ApplyAllClassOffsets();
            }
            break;
        case LiveObjKind::Shooting:
            if (!g_cls.ShootingInfo) {
                g_cls.ShootingInfo = reinterpret_cast<void*>(klass);
                ApplyAllClassOffsets();
            }
            break;
        case LiveObjKind::ShareData:
            if (!g_cls.ActorPlayerShareData) {
                g_cls.ActorPlayerShareData = reinterpret_cast<void*>(klass);
                ApplyAllClassOffsets();
            }
            break;
        case LiveObjKind::HeroShare:
            if (!g_cls.ActorHeroShareData) {
                g_cls.ActorHeroShareData = reinterpret_cast<void*>(klass);
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
                g_cls.NetPlayerData2 = reinterpret_cast<void*>(klass);
                ApplyAllClassOffsets();
            }
            break;
        case LiveObjKind::Battle:
            if (!g_cls.Battle) {
                g_cls.Battle = reinterpret_cast<void*>(klass);
                ApplyAllClassOffsets();
            }
            break;
    }
}

static void ScanClassesFromMaps() {
    const std::vector<MapsRegion> regions = ReadProcMaps();
    if (regions.empty()) {
        std::snprintf(g_scanDiag, sizeof(g_scanDiag), "Scan fail: no maps");
        return;
    }

    uint64_t strBase = 0;
    uint32_t strSize = 0;
    if (FindMetadataBaseFromMaps(regions)) {
        strBase = g_metaBase + g_metaStrOff;
        strSize = g_metaStrSize;
    } else if (!FindStringTableFallback(regions, strBase, strSize)) {
        std::snprintf(g_scanDiag, sizeof(g_scanDiag), "No meta (%zu rgns)", regions.size());
        SLOGE("ScanClassesFromMaps: %s", g_scanDiag);
        return;
    }

    const uint64_t strEnd  = strBase + strSize;
    const uint64_t strBaseU = strBase & kPtrTagMask;
    const uint64_t strEndU  = strEnd & kPtrTagMask;

    // Copy metadata string table safely so searches handle page boundaries cleanly
    std::vector<uint8_t> strBuf(strSize, 0);
    for (uint32_t off = 0; off < strSize; off += 256U) {
        const size_t chunk = std::min<size_t>(256U, strSize - off);
        MwrSafeReadMem(strBase + off, strBuf.data() + off, chunk);
    }

    struct TargetName {
        const char* name;
        const char* reqField;
        void**      slot;
        uint64_t    strAddrU;
    };
    const struct {
        const char* name;
        const char* reqField;
        void**      slot;
    } wanted[] = {
        {"PlayerManager",        "LocalHero",                  &g_cls.PlayerManager},
        {"LocalPlayerManager",   "_instance",                  &g_cls.LocalPlayerManager},
        {"CameraManager",        "mCamera",                    &g_cls.CameraManager},
        {"AntiCheatMgr",         "enableSceneWallCheck",       &g_cls.AntiCheatMgr},
        {"NetPlayerBase",        "isDestory",                  &g_cls.NetPlayerBase},
        {"Hero",                 "<mShareData>k__BackingField", &g_cls.Hero},
        {"NetPlayerData2",       "PlayerPosition",             &g_cls.NetPlayerData2},
        {"Battle",               "MaxHp",                      &g_cls.Battle},
        {"Player",               "_PlayerCamera",              &g_cls.Player},
        {"PlayerCamera",         "m_LookAngles",               &g_cls.PlayerCamera},
        {"CameraPhysicsHandler", "m_RotationSpring_Recoil",    &g_cls.CameraPhysicsHandler},
        {"PlayerMovement",       "m_GravityModule",            &g_cls.PlayerMovement},
        {"GravityModule",        "GravityMul",                 &g_cls.GravityModule},
        {"ActorPlayerShareData", "avatarController",           &g_cls.ActorPlayerShareData},
        {"ActorHeroShareData",   "<netData>k__BackingField",    &g_cls.ActorHeroShareData},
        {"AvatarRendererComp",   "RendererDict",               &g_cls.AvatarRendererComp},
        {"ProjectileWeapon",     "m_PW",                       &g_cls.ProjectileWeapon},
        {"ProjectileWeaponInfo", "Shooting",                   &g_cls.ProjectileWeaponInfo},
        {"ShootingInfo",         "FireDuration",               &g_cls.ShootingInfo},
        {"Object",               "m_CachedPtr",                &g_cls.UnityObject},
    };

    std::vector<TargetName> targets;
    for (const auto& w : wanted) {
        if (*w.slot != nullptr) continue;
        const size_t nlen = std::strlen(w.name);
        for (size_t i = 1; i + nlen + 1 <= strBuf.size(); ++i) {
            if (strBuf[i - 1] == 0 && strBuf[i + nlen] == 0 &&
                std::memcmp(strBuf.data() + i, w.name, nlen) == 0) {
                targets.push_back({w.name, w.reqField, w.slot, (strBase + i) & kPtrTagMask});
            }
        }
    }
    if (targets.empty()) {
        ApplyAllClassOffsets();
        return;
    }

    int nameHits = 0;
    int failRead = 0, failImg = 0, failNs = 0, failSt = 0;
    bool fieldsOffsetProbed = false;
    for (const auto& r : regions) {
        if (r.perms[0] != 'r' || r.perms[1] != 'w' || r.perms[3] != 'p') continue;
        if (ShouldSkipScanRegion(r) || !MwrIsValidAddrRange(r.start)) continue;
        if ((r.end - r.start) < 0x10000ULL) continue;

        for (uint64_t page = r.start; page < r.end; page += 4096ULL) {
            if (!MwrIsPageReadable(page)) continue;
            const size_t lim = std::min<size_t>(4096, static_cast<size_t>(r.end - page));
            const size_t nQ = lim / sizeof(uint64_t);
            const auto* q = reinterpret_cast<const uint64_t*>(page);

            for (size_t i = 0; i < nQ; ++i) {
                const uint64_t u = q[i] & kPtrTagMask;
                if (u < strBaseU || u >= strEndU) continue;

                for (const auto& t : targets) {
                    if (*(t.slot) != nullptr || u != t.strAddrU) continue;
                    ++nameHits;
                    const uint64_t nameFieldAddr = page + i * sizeof(uint64_t);
                    if (nameFieldAddr < 0x10ULL) {
                        ++failRead;
                        continue;
                    }
                    const uint64_t k = nameFieldAddr - 0x10ULL;

                    uint64_t img = 0, ns = 0, st = 0;
                    if (!MwrSafeRead<uint64_t>(k + 0x00, img) ||
                        !MwrSafeRead<uint64_t>(k + 0x18, ns) ||
                        !MwrSafeRead<uint64_t>(k + g_off.klass_static, st)) {
                        ++failRead;
                        continue;
                    }
                    const bool nsOk = ((ns & kPtrTagMask) >= strBaseU && (ns & kPtrTagMask) < strEndU);
                    if (!nsOk) {
                        ++failNs;
                        continue;
                    }
                    if (!MwrIsValidUserPtr(img)) {
                        ++failImg;
                        continue;
                    }
                    if (!fieldsOffsetProbed) {
                        DetectFieldsOffset(k);
                        fieldsOffsetProbed = true;
                    }
                    if (t.reqField) {
                        uint32_t dummyOff = 0;
                        if (!ResolveFieldFromChain(k, t.reqField, dummyOff, 4)) {
                            // Still allow core singletons if their fields weren't initialized cleanly yet
                            if (!MwrIsValidUserPtr(st)) {
                                ++failSt;
                                continue;
                            }
                        }
                    } else if (!MwrIsValidUserPtr(st)) {
                        uint64_t fields = SafeReadPtr(k + g_off.klass_fields);
                        uint64_t fParent = fields ? SafeReadPtr(fields + 0x10) : 0;
                        if (!fParent || (fParent & kPtrTagMask) != (k & kPtrTagMask)) {
                            ++failSt;
                            continue;
                        }
                    }

                    *(t.slot) = reinterpret_cast<void*>(k);
                    SLOGI("Maps Class %s @ 0x%lx static=0x%lx", t.name, (unsigned long)k, (unsigned long)st);
                }
            }

            bool allDone = true;
            for (const auto& t : targets) {
                if (*(t.slot) == nullptr) { allDone = false; break; }
            }
            if (allDone) {
                ApplyAllClassOffsets();
                std::snprintf(g_scanDiag, sizeof(g_scanDiag), "IL2CPP ok");
                return;
            }
        }
    }

    ApplyAllClassOffsets();
    if (g_cls.PlayerManager && g_cls.LocalPlayerManager && g_cls.CameraManager) {
        std::snprintf(g_scanDiag, sizeof(g_scanDiag), "IL2CPP ok");
    } else {
        std::snprintf(g_scanDiag, sizeof(g_scanDiag), "P%dL%dC%dA%d h%d r%d i%d n%d s%d",
                      g_cls.PlayerManager ? 1 : 0,
                      g_cls.LocalPlayerManager ? 1 : 0,
                      g_cls.CameraManager ? 1 : 0,
                      g_cls.AntiCheatMgr ? 1 : 0,
                      nameHits, failRead, failImg, failNs, failSt);
    }
}

static void ScanClasses() {
    if (!g_il2cpp.Init()) {
        ScanClassesFromMaps();
        return;
    }
    void* domain = g_il2cpp.domain_get();
    if (!domain) return;

    size_t asmCount = 0;
    const void** assemblies = g_il2cpp.domain_get_assemblies(domain, &asmCount);
    if (!assemblies || asmCount == 0) return;

    void* netPlayerBase = nullptr;
    void* heroCls = nullptr;
    void* netPlayerData2 = nullptr;
    void* hqPlayer = nullptr;
    void* battleCls = nullptr;

    for (size_t a = 0; a < asmCount; ++a) {
        void* img = g_il2cpp.assembly_get_image(assemblies[a]);
        if (!img) continue;
        const size_t clsCount = g_il2cpp.image_get_class_count(img);
        for (size_t c = 0; c < clsCount; ++c) {
            void* klass = g_il2cpp.image_get_class(img, c);
            if (!klass) continue;
            const char* cname = g_il2cpp.class_get_name(klass);
            if (!cname) continue;

            if (!g_cls.PlayerManager && std::strcmp(cname, "PlayerManager") == 0) {
                g_cls.PlayerManager = klass;
            } else if (!g_cls.LocalPlayerManager && std::strcmp(cname, "LocalPlayerManager") == 0) {
                g_cls.LocalPlayerManager = klass;
            } else if (!g_cls.CameraManager && std::strcmp(cname, "CameraManager") == 0) {
                g_cls.CameraManager = klass;
            } else if (!g_cls.AntiCheatMgr && std::strcmp(cname, "AntiCheatMgr") == 0) {
                g_cls.AntiCheatMgr = klass;
            } else if (!netPlayerBase && std::strcmp(cname, "NetPlayerBase") == 0) {
                netPlayerBase = klass;
            } else if (!heroCls && std::strcmp(cname, "Hero") == 0) {
                heroCls = klass;
            } else if (!netPlayerData2 && std::strcmp(cname, "NetPlayerData2") == 0) {
                netPlayerData2 = klass;
            } else if (!hqPlayer && std::strcmp(cname, "Player") == 0) {
                if (g_il2cpp.class_get_field_from_name &&
                    g_il2cpp.class_get_field_from_name(klass, "_PlayerCamera") != nullptr) {
                    hqPlayer = klass;
                }
            } else if (!battleCls && std::strcmp(cname, "Battle") == 0) {
                if (g_il2cpp.class_get_field_from_name &&
                    g_il2cpp.class_get_field_from_name(klass, "MaxHp") != nullptr) {
                    battleCls = klass;
                }
            }
        }
        if (g_cls.PlayerManager && g_cls.LocalPlayerManager && g_cls.CameraManager &&
            g_cls.AntiCheatMgr && netPlayerBase && heroCls && netPlayerData2 && hqPlayer && battleCls) {
            break;
        }
    }

    // Dynamically update offsets if fields are found (preserves defaults otherwise)
    if (g_cls.PlayerManager) {
        g_il2cpp.ResolveField(g_cls.PlayerManager, "LocalHero", g_off.pm_localHero);
        g_il2cpp.ResolveField(g_cls.PlayerManager, "_playerDict", g_off.pm_playerDict);
    }
    if (g_cls.LocalPlayerManager) {
        g_il2cpp.ResolveField(g_cls.LocalPlayerManager, "_instance", g_off.lpm_instance);
        g_il2cpp.ResolveField(g_cls.LocalPlayerManager, "DebugSpeedMulti", g_off.lpm_debugSpeedMulti);
        g_il2cpp.ResolveField(g_cls.LocalPlayerManager, "mLocalPlayer", g_off.lpm_localPlayer);
        g_il2cpp.ResolveField(g_cls.LocalPlayerManager, "curBindNetPlayerId", g_off.lpm_bindNetId);
    }
    if (g_cls.CameraManager) {
        g_il2cpp.ResolveField(g_cls.CameraManager, "instance", g_off.cm_instance);
        g_il2cpp.ResolveField(g_cls.CameraManager, "mCamera", g_off.cm_camera);
        g_il2cpp.ResolveField(g_cls.CameraManager, "mCameraTransform", g_off.cm_cameraTf);
    }
    if (g_cls.AntiCheatMgr) {
        g_il2cpp.ResolveField(g_cls.AntiCheatMgr, "enableSceneWallCheck", g_off.ac_sceneWall);
        g_il2cpp.ResolveField(g_cls.AntiCheatMgr, "enableValidTeleportCheck", g_off.ac_teleport);
        g_il2cpp.ResolveField(g_cls.AntiCheatMgr, "enableFlyCheckModel", g_off.ac_flyCheck);
        g_il2cpp.ResolveField(g_cls.AntiCheatMgr, "<BlockAceData2Report>k__BackingField", g_off.ac_block2Rep);
        g_il2cpp.ResolveField(g_cls.AntiCheatMgr, "<BlockAceData2Reply>k__BackingField", g_off.ac_block2Reply);
        g_il2cpp.ResolveField(g_cls.AntiCheatMgr, "<BlockAceData4Report>k__BackingField", g_off.ac_block4Rep);
    }
    if (netPlayerBase) {
        g_il2cpp.ResolveField(netPlayerBase, "<mTransform>k__BackingField", g_off.np_transform);
        g_il2cpp.ResolveField(netPlayerBase, "isDestory", g_off.np_isDestroy);
    }
    if (heroCls) {
        g_il2cpp.ResolveField(heroCls, "<mShareData>k__BackingField", g_off.np_shareData);
    }
    if (netPlayerData2) {
        g_il2cpp.ResolveField(netPlayerData2, "PlayerPosition", g_off.npd_playerPos);
        g_il2cpp.ResolveField(netPlayerData2, "battle", g_off.npd_battle);
        g_il2cpp.ResolveField(netPlayerData2, "<TeamIdStr>k__BackingField", g_off.npd_teamIdStr);
        g_il2cpp.ResolveField(netPlayerData2, "<PlayerNameStr>k__BackingField", g_off.npd_playerNameStr);
    }
    if (battleCls) {
        g_il2cpp.ResolveField(battleCls, "MaxHp", g_off.bat_maxHp);
        g_il2cpp.ResolveField(battleCls, "Hp", g_off.bat_hp);
    }
    if (hqPlayer) {
        g_il2cpp.ResolveField(hqPlayer, "_PlayerMovement", g_off.pl_movement);
        g_il2cpp.ResolveField(hqPlayer, "_PlayerCamera", g_off.pl_camera);
    }

    if (g_cls.PlayerManager && g_cls.LocalPlayerManager && g_cls.CameraManager && g_cls.AntiCheatMgr) {
        SLOGI("Classes: PM=%p LPM=%p CM=%p AC=%p",
              g_cls.PlayerManager, g_cls.LocalPlayerManager, g_cls.CameraManager, g_cls.AntiCheatMgr);
        return;
    }
    ScanClassesFromMaps();
    SLOGI("Classes: PM=%p LPM=%p CM=%p AC=%p",
          g_cls.PlayerManager, g_cls.LocalPlayerManager, g_cls.CameraManager, g_cls.AntiCheatMgr);
}

static std::string ReadIl2CppString(uint64_t strPtr, int maxLen = 32) {
    if (!MwrIsValidUserPtr(strPtr)) return {};
    int32_t len = SafeReadVal<int32_t>(strPtr + 0x10, 0);
    if (len <= 0 || len > 128) return {};
    int n = std::min(len, maxLen);
    uint16_t buf[64]{};
    if (!MwrSafeReadMem(strPtr + 0x14, buf, static_cast<size_t>(n) * 2)) return {};
    std::string out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        uint16_t ch = buf[i];
        if (ch >= 0x20 && ch <= 0x7e) {
            out.push_back(static_cast<char>(ch));
        } else if (ch == 0x0111) {
            out.push_back('d');
        } else if (ch == 0x0110) {
            out.push_back('D');
        }
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
    if (netId > 0) snprintf(buf, sizeof(buf), "Enemy #%d", netId);
    else snprintf(buf, sizeof(buf), "Enemy");
    return buf;
}

static std::string ReadPlayerTeamString(uint64_t share, uint64_t npd, bool isHero) {
    if (share) {
        const uint32_t baseOff = isHero ? g_off.ahs_baseData : g_off.aps_baseData;
        const uint64_t baseData = SafeReadPtr(share + baseOff);
        if (baseData) {
            EnsureLiveObjectOffsets(baseData, isHero ? LiveObjKind::HeroBaseData : LiveObjKind::PlayerBaseData);
            const uint32_t teamOff = isHero ? g_off.bd_heroTeamId : g_off.bd_playerTeamId;
            std::string s = ReadIl2CppString(SafeReadPtr(baseData + teamOff), 64);
            if (!s.empty()) return s;
        }
    }
    if (npd) {
        return ReadIl2CppString(SafeReadPtr(npd + g_off.npd_teamIdStr), 64);
    }
    return {};
}

static void ResolvePlayerRenderer(uint64_t share, uint64_t& outNatRend, uint64_t& outAv) {
    outNatRend = 0;
    outAv = 0;
    if (!MwrIsValidUserPtr(share)) return;
    outAv = SafeReadPtr(share + g_off.aps_avatarCtrl);
    if (!outAv) return;
    EnsureLiveObjectOffsets(outAv, LiveObjKind::AvatarCtrl);
    uint64_t mRend = SafeReadPtr(outAv + g_off.av_mRenderer);
    EnsureLiveObjectOffsets(mRend, LiveObjKind::AvatarRend);
    uint64_t rDict = mRend ? SafeReadPtr(mRend + g_off.ar_rendDict) : 0;
    EnsureLiveObjectOffsets(rDict, LiveObjKind::Dict);
    uint64_t re = rDict ? SafeReadPtr(rDict + g_off.dict_entries) : 0;
    if (!re) return;

    int32_t arrLen = SafeReadVal<int32_t>(re + g_off.arr_len, 0);
    int32_t scan = std::clamp(arrLen, 0, 10);
    for (int32_t j = 0; j < scan; ++j) {
        uint64_t reE = re + g_off.arr_data + static_cast<uint64_t>(j) * g_off.dictEntryStride;
        if (SafeReadVal<int32_t>(reE, -1) < 0) continue;
        uint64_t lst = SafeReadPtr(reE + g_off.dictEntryVal);
        if (!lst) continue;
        EnsureLiveObjectOffsets(lst, LiveObjKind::List);
        int32_t sz = SafeReadVal<int32_t>(lst + g_off.list_size, 0);
        if (sz <= 0) continue;
        uint64_t items = SafeReadPtr(lst + g_off.list_items);
        if (!items) continue;
        uint64_t rend0 = SafeReadPtr(items + g_off.arr_data);
        uint64_t natRend = rend0 ? SafeReadPtr(rend0 + g_off.obj_cachedPtr) : 0;
        if (natRend) {
            outNatRend = natRend;
            return;
        }
    }
}

static void RestoreWeaponOriginals() {
    for (const auto& kv : g_orig.shootMap) {
        SafeWriteVal<float>(kv.second.ptr + g_off.sh_fireDur, kv.second.fireDur);
        SafeWriteVal<float>(kv.second.ptr + g_off.sh_fireDurAuto, kv.second.fireDurAuto);
    }
    g_orig.shootMap.clear();
    for (const auto& kv : g_orig.spreadMap) {
        SafeWriteVal<float>(kv.second.ptr + g_off.pw_accurateRatio, kv.second.accRatio);
    }
    g_orig.spreadMap.clear();
}

static void RestoreOriginals() {
    RestoreWeaponOriginals();
    if (g_orig.acCaptured && g_cls.AntiCheatMgr) {
        uint64_t st = g_il2cpp.GetStaticData(g_cls.AntiCheatMgr, g_off);
        if (MwrIsValidUserPtr(st)) {
            SafeWriteVal<uint8_t>(st + g_off.ac_sceneWall, g_orig.acBytes[0]);
            SafeWriteVal<uint8_t>(st + g_off.ac_teleport, g_orig.acBytes[1]);
            SafeWriteVal<uint8_t>(st + g_off.ac_flyCheck, g_orig.acBytes[2]);
            SafeWriteVal<uint8_t>(st + g_off.ac_block2Rep, g_orig.acBytes[3]);
            SafeWriteVal<uint8_t>(st + g_off.ac_block2Reply, g_orig.acBytes[4]);
            SafeWriteVal<uint8_t>(st + g_off.ac_block4Rep, g_orig.acBytes[5]);
        }
    }
    if (g_cls.LocalPlayerManager) {
        uint64_t lpmSt = g_il2cpp.GetStaticData(g_cls.LocalPlayerManager, g_off);
        uint64_t lpm = lpmSt ? SafeReadPtr(lpmSt + g_off.lpm_instance) : 0;
        if (lpm) {
            if (g_orig.speedCaptured) {
                SafeWriteVal<float>(lpm + g_off.lpm_debugSpeedMulti, g_orig.debugSpeedMulti);
            }
            uint64_t lp = SafeReadPtr(lpm + g_off.lpm_localPlayer);
            if (lp) {
                uint64_t pc = SafeReadPtr(lp + g_off.pl_camera);
                if (pc && g_orig.recoilCaptured) {
                    SafeWriteVal<uint8_t>(pc + g_off.pc_recoilEnabled, g_orig.recoilEnabled);
                }
                uint64_t pm = SafeReadPtr(lp + g_off.pl_movement);
                if (pm && g_orig.gravCaptured) {
                    uint64_t gm = SafeReadPtr(pm + g_off.pmov_gravityMod);
                    if (gm) {
                        SafeWriteVal<float>(gm + g_off.gm_gravityMul, g_orig.gravityMul);
                        SafeWriteVal<float>(gm + g_off.gm_fallSpeedMul, g_orig.fallSpeedMul);
                    }
                }
            }
        }
    }
    g_lastAc = g_lastSpeed = g_lastGrav = g_lastRecoil = g_lastSpread = g_lastRapid = false;
}

static int RunFeatureTick() {
    ModState* st = imgui_get_state();
    ModTelemetry* tele = imgui_get_telemetry();
    EspSnapshot* esp = imgui_get_esp_snapshot();
    if (!st || !tele || !esp) return 120;

    if (st->reqKill) {
        return -1; // signal clean unload
    }

    if (st->reqRescan) {
        st->reqRescan = 0;
        g_cls = Classes{};
        g_resolvedObjKlasses.clear();
        g_playerCache.clear();
        g_cachedLocalHero = 0;
        g_cachedLocalTeamStr.clear();
        ScanClasses();
    }

    if (st->reqReset) {
        st->reqReset = 0;
        RestoreOriginals();
    }

    static auto s_lastRescan = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    if (!g_cls.PlayerManager || !g_cls.CameraManager || !g_cls.LocalPlayerManager) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastRescan).count() >= 3000) {
            s_lastRescan = now;
            ScanClasses();
        }
    }

    const bool menuOpen = (imgui_is_menu_open() != 0);
    const bool anyModOn = (st->espEnabled || st->aimbot || st->noRecoil || st->noSpread ||
                           st->rapidFire || st->speedHack || st->lowGravity || st->acBypass);

    // 1. CameraManager
    uint64_t camMgrInst = 0, nativeCam = 0, natCamTf = 0;
    if (g_cls.CameraManager) {
        uint64_t cmSt = g_il2cpp.GetStaticData(g_cls.CameraManager, g_off);
        camMgrInst = cmSt ? SafeReadPtr(cmSt + g_off.cm_instance) : 0;
        if (camMgrInst) {
            uint64_t mCam = SafeReadPtr(camMgrInst + g_off.cm_camera);
            if (mCam) nativeCam = SafeReadPtr(mCam + g_off.obj_cachedPtr);
            uint64_t mCamTf = SafeReadPtr(camMgrInst + g_off.cm_cameraTf);
            if (mCamTf) natCamTf = SafeReadPtr(mCamTf + g_off.obj_cachedPtr);
        }
    }

    // 2. LocalPlayerManager & PlayerManager
    uint64_t lpmInst = 0, localPlayer = 0, playerCam = 0, recoilSpring = 0, localHero = 0;
    int32_t localNetId = -1;
    float localHp = 0.0f, localMaxHp = 0.0f;

    if (g_cls.LocalPlayerManager) {
        uint64_t lpmSt = g_il2cpp.GetStaticData(g_cls.LocalPlayerManager, g_off);
        lpmInst = lpmSt ? SafeReadPtr(lpmSt + g_off.lpm_instance) : 0;
        if (lpmInst) {
            localPlayer = SafeReadPtr(lpmInst + g_off.lpm_localPlayer);
            localNetId = SafeReadVal<int32_t>(lpmInst + g_off.lpm_bindNetId, -1);
            if (localPlayer) {
                EnsureLiveObjectOffsets(localPlayer, LiveObjKind::Player);
                playerCam = SafeReadPtr(localPlayer + g_off.pl_camera);
                if (playerCam) {
                    EnsureLiveObjectOffsets(playerCam, LiveObjKind::PlayerCam);
                    uint64_t cph = SafeReadPtr(playerCam + g_off.pc_physHandler);
                    if (cph) {
                        EnsureLiveObjectOffsets(cph, LiveObjKind::CamPhys);
                        recoilSpring = SafeReadPtr(cph + g_off.cph_recoilSpring);
                        if (recoilSpring) EnsureLiveObjectOffsets(recoilSpring, LiveObjKind::RecoilSpring);
                    }
                }
            }
        }
    }

    const bool needEntities = (st->espEnabled || st->aimbot || menuOpen);
    uint64_t pmStatic = 0;
    int32_t dictCount = 0, enemyCount = 0;

    static EspEntity s_ents[kMaxEspEntities];
    int entOutCount = 0;

    static uint32_t s_teamRefreshTick = 0;
    ++s_teamRefreshTick;
    const bool periodicTeamRefresh = (s_teamRefreshTick % 16U) == 0;

    if (g_cls.PlayerManager) {
        pmStatic = g_il2cpp.GetStaticData(g_cls.PlayerManager, g_off);
        if (MwrIsValidUserPtr(pmStatic)) {
            localHero = SafeReadPtr(pmStatic + g_off.pm_localHero);
            if (localHero) {
                uint64_t hShare = SafeReadPtr(localHero + g_off.np_shareData);
                if (hShare) EnsureLiveObjectOffsets(hShare, LiveObjKind::HeroShare);
                uint64_t hNpd = hShare ? SafeReadPtr(hShare + g_off.ahs_netData) : 0;
                if (hNpd) EnsureLiveObjectOffsets(hNpd, LiveObjKind::NetPlayerData);

                if (localHero != g_cachedLocalHero) {
                    g_cachedLocalHero = localHero;
                    g_cachedLocalTeamStr = ReadPlayerTeamString(hShare, hNpd, true);
                    g_playerCache.clear();
                } else if (g_cachedLocalTeamStr.empty() || periodicTeamRefresh) {
                    std::string latest = ReadPlayerTeamString(hShare, hNpd, true);
                    if (!latest.empty()) g_cachedLocalTeamStr = std::move(latest);
                }

                if (hNpd && menuOpen) {
                    uint64_t bat = SafeReadPtr(hNpd + g_off.npd_battle);
                    if (bat) {
                        EnsureLiveObjectOffsets(bat, LiveObjKind::Battle);
                        uint64_t hpV = SafeReadPtr(bat + g_off.bat_hp);
                        uint64_t mhpV = SafeReadPtr(bat + g_off.bat_maxHp);
                        if (hpV) {
                            EnsureLiveObjectOffsets(hpV, LiveObjKind::NetVar);
                            localHp = static_cast<float>(SafeReadVal<int32_t>(hpV + g_off.nv_internalValue, 0));
                        }
                        if (mhpV) localMaxHp = static_cast<float>(SafeReadVal<int32_t>(mhpV + g_off.nv_internalValue, 0));
                    }
                }
            } else if (g_cachedLocalHero != 0) {
                g_cachedLocalHero = 0;
                g_cachedLocalTeamStr.clear();
                g_playerCache.clear();
            }

            uint64_t dict = SafeReadPtr(pmStatic + g_off.pm_playerDict);
            if (dict) {
                EnsureLiveObjectOffsets(dict, LiveObjKind::Dict);
                dictCount = SafeReadVal<int32_t>(dict + g_off.dict_count, 0);
                if (needEntities && dictCount > 0 && dictCount < 256) {
                    uint64_t entries = SafeReadPtr(dict + g_off.dict_entries);
                    if (entries) {
                        int32_t arrLen = SafeReadVal<int32_t>(entries + g_off.arr_len, 0);
                        int32_t scanLen = std::clamp(arrLen, 0, 200);
                        for (int32_t i = 0; i < scanLen; ++i) {
                            uint64_t entAddr = entries + g_off.arr_data + static_cast<uint64_t>(i) * g_off.dictEntryStride;
                            if (SafeReadVal<int32_t>(entAddr, -1) < 0) continue;
                            uint64_t netPlayer = SafeReadPtr(entAddr + g_off.dictEntryVal);
                            if (!netPlayer || netPlayer == localHero) continue;
                            if (SafeReadVal<uint8_t>(netPlayer + g_off.np_isDestroy, 1) != 0) continue;

                            uint64_t share = SafeReadPtr(netPlayer + g_off.np_shareData);
                            if (share) EnsureLiveObjectOffsets(share, LiveObjKind::ShareData);
                            uint64_t npd = share ? SafeReadPtr(share + g_off.aps_netData) : 0;
                            if (npd) EnsureLiveObjectOffsets(npd, LiveObjKind::NetPlayerData);

                            auto it = g_playerCache.find(netPlayer);
                            if (it == g_playerCache.end()) {
                                PlayerCacheEntry ce;
                                ce.netId = share ? SafeReadVal<int32_t>(share + g_off.aps_netPlayerId, -1) : -1;
                                ce.teamStr = ReadPlayerTeamString(share, npd, false);
                                std::string rawName;
                                if (npd) {
                                    rawName = ReadIl2CppString(SafeReadPtr(npd + g_off.npd_playerNameStr), 32);
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
                                ? SafeReadVal<uint8_t>(it->second.av + g_off.av_renderShow, 1)
                                : 1;

                            if (localNetId > 0 && it->second.netId == localNetId) continue;

                            uint64_t mTf = SafeReadPtr(netPlayer + g_off.np_transform);
                            if (!mTf && share) mTf = SafeReadPtr(share + g_off.aps_playerTrans);
                            uint64_t natTf = mTf ? SafeReadPtr(mTf + g_off.obj_cachedPtr) : 0;

                            float hp = 100.0f, maxHp = 100.0f;
                            float fx = 0.0f, fy = 0.0f, fz = 0.0f;
                            if (npd) {
                                uint64_t bat = SafeReadPtr(npd + g_off.npd_battle);
                                if (bat) {
                                    uint64_t hpV = SafeReadPtr(bat + g_off.bat_hp);
                                    uint64_t mhpV = SafeReadPtr(bat + g_off.bat_maxHp);
                                    if (hpV) hp = static_cast<float>(SafeReadVal<int32_t>(hpV + g_off.nv_internalValue, 100));
                                    if (mhpV) maxHp = static_cast<float>(SafeReadVal<int32_t>(mhpV + g_off.nv_internalValue, 100));
                                }
                                if (!natTf) {
                                    uint64_t pv = SafeReadPtr(npd + g_off.npd_playerPos);
                                    if (pv) {
                                        fx = SafeReadVal<float>(pv + g_off.nv_internalValue, 0.0f);
                                        fy = SafeReadVal<float>(pv + g_off.nv_internalValue + 4, 0.0f);
                                        fz = SafeReadVal<float>(pv + g_off.nv_internalValue + 8, 0.0f);
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

    if (st->espEnabled || st->aimbot || st->noRecoil) {
        const uint32_t curSeq = __atomic_load_n(&esp->seq, __ATOMIC_RELAXED);
        const uint32_t busySeq = (curSeq + 1u) | 1u;
        __atomic_store_n(&esp->seq, busySeq, __ATOMIC_RELEASE);
        esp->count = entOutCount;
        esp->nativeCamPtr = nativeCam;
        esp->playerCameraPtr = playerCam;
        esp->camTfPtr = natCamTf;
        esp->recoilSpringPtr = recoilSpring;
        if (entOutCount > 0) {
            std::memcpy(esp->entities, s_ents, static_cast<size_t>(entOutCount) * sizeof(EspEntity));
        }
        __atomic_store_n(&esp->seq, busySeq + 1u, __ATOMIC_RELEASE);
    }

    // 3. AntiCheatMgr telemetry disable
    if (g_cls.AntiCheatMgr) {
        uint64_t acSt = g_il2cpp.GetStaticData(g_cls.AntiCheatMgr, g_off);
        if (MwrIsValidUserPtr(acSt)) {
            if (!g_orig.acCaptured) {
                g_orig.acBytes[0] = SafeReadVal<uint8_t>(acSt + g_off.ac_sceneWall, 1);
                g_orig.acBytes[1] = SafeReadVal<uint8_t>(acSt + g_off.ac_teleport, 1);
                g_orig.acBytes[2] = SafeReadVal<uint8_t>(acSt + g_off.ac_flyCheck, 1);
                g_orig.acBytes[3] = SafeReadVal<uint8_t>(acSt + g_off.ac_block2Rep, 0);
                g_orig.acBytes[4] = SafeReadVal<uint8_t>(acSt + g_off.ac_block2Reply, 0);
                g_orig.acBytes[5] = SafeReadVal<uint8_t>(acSt + g_off.ac_block4Rep, 0);
                g_orig.acCaptured = true;
            }
            if (st->acBypass) {
                SafeWriteVal<uint8_t>(acSt + g_off.ac_sceneWall, 0);
                SafeWriteVal<uint8_t>(acSt + g_off.ac_teleport, 0);
                SafeWriteVal<uint8_t>(acSt + g_off.ac_flyCheck, 0);
                SafeWriteVal<uint8_t>(acSt + g_off.ac_block2Rep, 1);
                SafeWriteVal<uint8_t>(acSt + g_off.ac_block2Reply, 1);
                SafeWriteVal<uint8_t>(acSt + g_off.ac_block4Rep, 1);
                g_lastAc = true;
            } else if (g_lastAc) {
                SafeWriteVal<uint8_t>(acSt + g_off.ac_sceneWall, g_orig.acBytes[0]);
                SafeWriteVal<uint8_t>(acSt + g_off.ac_teleport, g_orig.acBytes[1]);
                SafeWriteVal<uint8_t>(acSt + g_off.ac_flyCheck, g_orig.acBytes[2]);
                SafeWriteVal<uint8_t>(acSt + g_off.ac_block2Rep, g_orig.acBytes[3]);
                SafeWriteVal<uint8_t>(acSt + g_off.ac_block2Reply, g_orig.acBytes[4]);
                SafeWriteVal<uint8_t>(acSt + g_off.ac_block4Rep, g_orig.acBytes[5]);
                g_lastAc = false;
            }
        }
    }

    // 4. LocalPlayer combat & movement
    if (lpmInst) {
        if (!g_orig.speedCaptured) {
            float spd = SafeReadVal<float>(lpmInst + g_off.lpm_debugSpeedMulti, 1.0f);
            g_orig.debugSpeedMulti = (spd > 0.01f) ? spd : 1.0f;
            g_orig.speedCaptured = true;
        }
        if (st->speedHack) {
            SafeWriteVal<float>(lpmInst + g_off.lpm_debugSpeedMulti, g_orig.debugSpeedMulti * st->speedMult);
            g_lastSpeed = true;
        } else if (g_lastSpeed) {
            SafeWriteVal<float>(lpmInst + g_off.lpm_debugSpeedMulti, g_orig.debugSpeedMulti);
            g_lastSpeed = false;
        }
    }

    if (localPlayer) {
        if (playerCam) {
            if (!g_orig.recoilCaptured) {
                g_orig.recoilEnabled = SafeReadVal<uint8_t>(playerCam + g_off.pc_recoilEnabled, 1);
                g_orig.recoilCaptured = true;
            }
            if (st->noRecoil) {
                SafeWriteVal<uint8_t>(playerCam + g_off.pc_recoilEnabled, 0);
                SafeWriteVal<float>(playerCam + g_off.pc_recoilAngles, 0.0f);
                SafeWriteVal<float>(playerCam + g_off.pc_recoilAngles + 4, 0.0f);
                g_lastRecoil = true;
            } else if (g_lastRecoil) {
                SafeWriteVal<uint8_t>(playerCam + g_off.pc_recoilEnabled, g_orig.recoilEnabled);
                g_lastRecoil = false;
            }
        }

        if (st->lowGravity || g_lastGrav) {
            uint64_t pm = SafeReadPtr(localPlayer + g_off.pl_movement);
            if (pm) EnsureLiveObjectOffsets(pm, LiveObjKind::Movement);
            uint64_t gm = pm ? SafeReadPtr(pm + g_off.pmov_gravityMod) : 0;
            if (gm) {
                EnsureLiveObjectOffsets(gm, LiveObjKind::Gravity);
                if (!g_orig.gravCaptured) {
                    g_orig.gravityMul = SafeReadVal<float>(gm + g_off.gm_gravityMul, 1.0f);
                    g_orig.fallSpeedMul = SafeReadVal<float>(gm + g_off.gm_fallSpeedMul, 1.0f);
                    g_orig.gravCaptured = true;
                }
                if (st->lowGravity) {
                    SafeWriteVal<float>(gm + g_off.gm_gravityMul, g_orig.gravityMul * st->gravityScale);
                    SafeWriteVal<float>(gm + g_off.gm_fallSpeedMul, g_orig.fallSpeedMul * st->gravityScale);
                    g_lastGrav = true;
                } else if (g_lastGrav) {
                    SafeWriteVal<float>(gm + g_off.gm_gravityMul, g_orig.gravityMul);
                    SafeWriteVal<float>(gm + g_off.gm_fallSpeedMul, g_orig.fallSpeedMul);
                    g_lastGrav = false;
                }
            }
        }

        if (st->noSpread || st->rapidFire || g_lastSpread || g_lastRapid) {
            uint64_t eqVal = SafeReadPtr(localPlayer + g_off.pl_equippedItemVal);
            if (eqVal) EnsureLiveObjectOffsets(eqVal, LiveObjKind::QueuedVal);
            uint64_t gun = eqVal ? SafeReadPtr(eqVal + g_off.val_currentValue) : 0;
            if (gun) {
                EnsureLiveObjectOffsets(gun, LiveObjKind::Gun);
                if (st->noSpread) {
                    if (g_orig.spreadMap.find(gun) == g_orig.spreadMap.end()) {
                        g_orig.spreadMap[gun] = {gun, SafeReadVal<float>(gun + g_off.pw_accurateRatio, 1.0f)};
                    }
                    SafeWriteVal<float>(gun + g_off.pw_curAccurate, 0.0f);
                    SafeWriteVal<float>(gun + g_off.pw_shootAccurate, 0.0f);
                    SafeWriteVal<float>(gun + g_off.pw_accurateRatio, 0.0f);
                    g_lastSpread = true;
                } else if (g_lastSpread) {
                    for (const auto& kv : g_orig.spreadMap) {
                        SafeWriteVal<float>(kv.second.ptr + g_off.pw_accurateRatio, kv.second.accRatio);
                    }
                    g_orig.spreadMap.clear();
                    g_lastSpread = false;
                }

                if (st->rapidFire || g_lastRapid) {
                    uint64_t mPW = SafeReadPtr(gun + g_off.pw_mPW);
                    if (mPW) EnsureLiveObjectOffsets(mPW, LiveObjKind::WeaponInfo);
                    uint64_t sh = mPW ? SafeReadPtr(mPW + g_off.pwi_shooting) : 0;
                    if (sh) EnsureLiveObjectOffsets(sh, LiveObjKind::Shooting);
                    if (st->rapidFire) {
                        SafeWriteVal<float>(gun + g_off.eq_nextUseTime, 0.0f);
                        SafeWriteVal<float>(gun + g_off.pw_fireDurTimer, 0.0f);
                        if (sh) {
                            if (g_orig.shootMap.find(sh) == g_orig.shootMap.end()) {
                                g_orig.shootMap[sh] = {
                                    sh,
                                    SafeReadVal<float>(sh + g_off.sh_fireDur, 0.1f),
                                    SafeReadVal<float>(sh + g_off.sh_fireDurAuto, 0.1f),
                                };
                            }
                            SafeWriteVal<float>(sh + g_off.sh_fireDur, 0.025f);
                            SafeWriteVal<float>(sh + g_off.sh_fireDurAuto, 0.045f);
                        }
                        g_lastRapid = true;
                    } else if (g_lastRapid) {
                        for (const auto& kv : g_orig.shootMap) {
                            SafeWriteVal<float>(kv.second.ptr + g_off.sh_fireDur, kv.second.fireDur);
                            SafeWriteVal<float>(kv.second.ptr + g_off.sh_fireDurAuto, kv.second.fireDurAuto);
                        }
                        g_orig.shootMap.clear();
                        g_lastRapid = false;
                    }
                }
            }
        }
    }

    // 5. Update Telemetry
    tele->playerMgrAddr = pmStatic;
    tele->localHeroAddr = localHero;
    tele->localPlayerAddr = localPlayer;
    tele->camMgrAddr = camMgrInst;
    tele->nativeCamAddr = nativeCam;
    if (menuOpen) {
        tele->playerCount = dictCount;
        tele->enemyCount = enemyCount;
        tele->localHp = localHp;
        tele->localMaxHp = localMaxHp;
        tele->localTeamId = localNetId;
        const char* status = localPlayer ? "Active in match"
                           : (camMgrInst ? "Waiting for match"
                           : (g_cls.CameraManager ? "IL2CPP ready (wait)" : g_scanDiag));
        std::memset(tele->statusText, 0, sizeof(tele->statusText));
        std::strncpy(tele->statusText, status, sizeof(tele->statusText) - 1);
    }

    return (localPlayer && anyModOn) ? 25 : 120;
}

static void* StandaloneWorker(void*) {
    // Wait briefly so Frida launcher can claim lifecycle if attached
    usleep(350000);
    if (g_fridaControlled.load()) {
        return nullptr;
    }

    SLOGI("Entering standalone native mode (no Frida).");

    // Resolve JNI VM for DecorView touch scaling
    JavaVM* jvm = nullptr;
    bool jniAttached = false;
    if (void* art = dlopen("libart.so", RTLD_NOLOAD)) {
        using FnGetVMs = jint (*)(JavaVM**, jsize, jsize*);
        auto fnGetVMs = reinterpret_cast<FnGetVMs>(dlsym(art, "JNI_GetCreatedJavaVMs"));
        if (fnGetVMs) {
            jsize nVMs = 0;
            fnGetVMs(&jvm, 1, &nVMs);
            if (nVMs <= 0) jvm = nullptr;
        }
    }

    // Install PLT/GOT hooks for eglSwapBuffers and InputConsumer::consume
    bool swapHooked = false;
    bool touchHooked = false;
    auto lastHookCheck = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    auto lastDecorCheck = std::chrono::steady_clock::now() - std::chrono::seconds(5);

    while (true) {
        auto now = std::chrono::steady_clock::now();
        if ((!swapHooked || !touchHooked) &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHookCheck).count() >= 500) {
            lastHookCheck = now;
            if (!swapHooked) {
                if (InstallPltHook("libunity.so", "eglSwapBuffers",
                                   reinterpret_cast<void*>(&HookEglSwapBuffers),
                                   reinterpret_cast<void**>(&g_origEglSwap)) > 0) {
                    if (!g_origEglSwap) {
                        if (void* egl = dlopen("libEGL.so", RTLD_NOLOAD)) {
                            g_origEglSwap = reinterpret_cast<FnEglSwapBuffers>(dlsym(egl, "eglSwapBuffers"));
                        }
                    }
                    swapHooked = (g_origEglSwap != nullptr);
                }
            }
            if (!touchHooked) {
                const char* kConsumeSym =
                    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE";
                if (InstallPltHook("libandroid_runtime.so", kConsumeSym,
                                   reinterpret_cast<void*>(&HookInputConsume),
                                   reinterpret_cast<void**>(&g_origInputConsume)) > 0) {
                    if (!g_origInputConsume) {
                        if (void* inp = dlopen("libinput.so", RTLD_NOLOAD)) {
                            g_origInputConsume = reinterpret_cast<FnInputConsume>(dlsym(inp, kConsumeSym));
                        }
                    }
                    touchHooked = (g_origInputConsume != nullptr);
                }
            }
        }

        if (jvm && std::chrono::duration_cast<std::chrono::milliseconds>(now - lastDecorCheck).count() >= 2000) {
            lastDecorCheck = now;
            RefreshDecorSize(jvm, jniAttached);
        }

        int delayMs = RunFeatureTick();
        if (delayMs < 0) {
            break;
        }
        usleep(static_cast<useconds_t>(delayMs) * 1000U);
    }

    SLOGI("Unloading standalone mod menu...");
    imgui_request_shutdown();
    for (int i = 0; i < 50 && imgui_is_shutdown_done() == 0; ++i) {
        usleep(10000);
    }
    g_detaching.store(true);
    usleep(50000);
    RestoreOriginals();
    RestorePltHooks();
    if (jvm && jniAttached) {
        jvm->DetachCurrentThread();
    }
    SLOGI("Standalone unload complete.");
    return nullptr;
}

__attribute__((constructor))
static void OnMwrImGuiLoad() {
    pthread_t th{};
    if (pthread_create(&th, nullptr, StandaloneWorker, nullptr) == 0) {
        pthread_detach(th);
    }
}

} // namespace

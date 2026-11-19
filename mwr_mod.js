'use strict';

// ============================================================================
// Mini World Royale (com.sea.miniworldroyale.sparkgame)
// Unity IL2CPP v24 (arm64 under libhoudini) + x86_64 Dear ImGui GLES3 Overlay
// Optimized: 120Hz C++ ESP+Aimbot on GL thread; adaptive low-overhead JS tick
// ============================================================================

const CONFIG = {
    activeTickMs: 25,     // 40Hz when in-match mods/ESP active (C++ handles 120Hz ESP+Aim)
    idleTickMs: 120,      // ~8Hz when idle/lobby/mods off
    rescanIntervalMs: 3000,
    soPath: '/data/data/com.sea.miniworldroyale.sparkgame/cache/libmwr_imgui.so',
    waitForStage: false,
};

// ModState offsets (must match src/mwr_imgui.cpp)
const ST = {
    aimbot: 0x00,
    aimFovPx: 0x04,
    aimSmooth: 0x08,
    drawFov: 0x0C,
    aimTeamCheck: 0x10,
    noRecoil: 0x14,
    noSpread: 0x18,
    rapidFire: 0x1C,
    espEnabled: 0x20,
    espBox: 0x24,
    espLine: 0x28,
    espHp: 0x2C,
    espInfo: 0x30,
    espTeamCheck: 0x34,
    espMaxDist: 0x38,
    speedHack: 0x3C,
    speedMult: 0x40,
    lowGravity: 0x44,
    gravityScale: 0x48,
    acBypass: 0x4C,
    reqRescan: 0x50,
    reqReset: 0x54,
    reqKill: 0x58,
    aimVisCheck: 0x5C,
    aimPriority: 0x60,
    aimTracer: 0x64,
};

// ModTelemetry offsets (must match src/mwr_imgui.cpp)
const TELE = {
    playerMgrAddr: 0x00,
    localHeroAddr: 0x08,
    localPlayerAddr: 0x10,
    camMgrAddr: 0x18,
    nativeCamAddr: 0x20,
    playerCount: 0x28,
    enemyCount: 0x2C,
    localX: 0x30,
    localY: 0x34,
    localZ: 0x38,
    localHp: 0x3C,
    localMaxHp: 0x40,
    localTeamId: 0x44,
    statusText: 0x48,
};

// EspSnapshot offsets (must match src/mwr_imgui.cpp)
const ESP = {
    seq: 0x00,
    count: 0x04,
    nativeCamPtr: 0x08,
    playerCameraPtr: 0x10,
    camTfPtr: 0x18,
    recoilSpringPtr: 0x20,
    fallbackVp: 0x28,
    camPos: 0x68,
    entities: 0x78,
    entStride: 0x40,
    MAX_ENT: 64,
};

const ENT = {
    nativeTfPtr: 0x00,
    nativeRendPtr: 0x08,
    x: 0x10,
    y: 0x14,
    z: 0x18,
    hp: 0x1C,
    maxHp: 0x20,
    isEnemy: 0x24,
    renderShow: 0x28,
    name: 0x2C,
};

// Verified IL2CPP v24 offsets
const OFF = {
    klass_name: 0x10,
    klass_ns: 0x18,
    klass_static: 0xb8,

    obj_cachedPtr: 0x10,
    natCam_vp: 0xdc,

    dict_entries: 0x18,
    dict_count: 0x20,
    arr_len: 0x18,
    arr_data: 0x20,
    dictEntryStride: 24,
    dictEntryVal: 0x10,

    pm_localHero: 0x00,
    pm_playerDict: 0x18,

    lpm_instance: 0x00,
    lpm_debugSpeedMulti: 0x10,
    lpm_localPlayer: 0x18,
    lpm_bindNetId: 0x20,

    cm_instance: 0x00,
    cm_camera: 0x18,
    cm_cameraTf: 0x28,

    ac_sceneWall: 0x00,
    ac_teleport: 0x01,
    ac_flyCheck: 0x08,
    ac_block2Rep: 0x09,
    ac_block2Reply: 0x0a,
    ac_block4Rep: 0x0b,

    np_transform: 0x30,
    np_isDestroy: 0x38,
    np_shareData: 0xc0,

    aps_playerTrans: 0x58,
    aps_netData: 0x70,
    aps_avatarCtrl: 0x78,
    aps_netPlayerId: 0x88,

    av_mRenderer: 0xd0,
    av_renderShow: 0xe0,
    ar_rendDict: 0x10,
    list_items: 0x10,
    list_size: 0x18,

    ahs_netData: 0x28,

    npd_playerPos: 0x80,
    npd_battle: 0x1d0,
    npd_teamIdStr: 0x2e0,
    npd_playerNameStr: 0x2f0,

    nv_internalValue: 0x48,
    bat_maxHp: 0x28,
    bat_hp: 0x38,

    pl_equippedItemVal: 0x1d8,
    val_currentValue: 0x20,
    pl_movement: 0x4d8,
    pl_camera: 0x4e8,

    pc_physHandler: 0x28,
    pc_recoilAngles: 0x58,
    pc_recoilEnabled: 0x60,
    cph_recoilSpring: 0x68,

    pmov_gravityMod: 0xd8,
    gm_gravityMul: 0x14,
    gm_fallSpeedMul: 0x1c,

    eq_nextUseTime: 0xd4,
    pw_curAccurate: 0x110,
    pw_shootAccurate: 0x114,
    pw_accurateRatio: 0x11c,
    pw_mPW: 0x140,
    pwi_shooting: 0x38,
    sh_fireDur: 0x14,
    sh_fireDurAuto: 0x18,
    pw_fireDurTimer: 0x174,
};

let gMetaBase = null;
let gClasses = {
    PlayerManager: null,
    LocalPlayerManager: null,
    CameraManager: null,
    AntiCheatMgr: null,
};
let gLastScanMs = 0;

// Cached per-match player identity metadata (avoids UTF-16 + regex per tick)
let gPlayerCache = {};
let gCachedLocalHeroKey = '';
let gCachedLocalTeamStr = '';

const orig = {
    speedCaptured: false,
    debugSpeedMulti: 1.0,
    gravCaptured: false,
    gravityMul: 1.0,
    fallSpeedMul: 1.0,
    recoilCaptured: false,
    recoilEnabled: 1,
    acCaptured: false,
    acBytes: [1, 1, 1, 0, 0, 0],
    shootingMap: {},
    spreadMap: {},
};

let gImGuiMod = null;
let gStatePtr = null;
let gTelePtr = null;
let gEspPtr = null;
let gSwapHook = null;
let gSwapDetaching = false;
let gTouchHooked = false;
let gTickTimer = null;
let gDestroyed = false;

let fnOnSwap = null;
let fnOnTouch = null;
let fnReqShutdown = null;
let fnIsShutdownDone = null;
let fnToggleMenu = null;
let fnSetMenuOpen = null;
let fnIsMenuOpen = null;
let fnGetSurfaceSize = null;
let gSurfSizeBuf = null;

const PTR_TAG_MASK = uint64('0x00ffffffffffffff');
const PTR_MIN = uint64('0x10000000');
const PTR_MAX = uint64('0x0000ffffffffffff');

function isValidPtr(p) {
    if (!p || p.isNull()) return false;
    const untagged = uint64(p.toString()).and(PTR_TAG_MASK);
    return untagged.compare(PTR_MIN) >= 0 && untagged.compare(PTR_MAX) < 0;
}

function safeReadPtr(p) {
    if (!isValidPtr(p)) return ptr(0);
    try { return p.readPointer(); } catch (_) { return ptr(0); }
}

function sanitizeLabel(s, netId) {
    if (!s) return netId > 0 ? ('Enemy #' + netId) : 'Enemy';
    let ascii = '';
    try {
        ascii = s.normalize('NFD').replace(/[\u0300-\u036f]/g, '').replace(/đ/g, 'd').replace(/Đ/g, 'D');
    } catch (_) {
        ascii = s;
    }
    ascii = ascii.replace(/[^\x20-\x7e]/g, '').trim();
    if (ascii.length < 2) {
        return netId > 0 ? ('Enemy #' + netId) : 'Enemy';
    }
    return ascii.slice(0, 22);
}

function readIl2CppString(p, maxLen) {
    if (!isValidPtr(p)) return '';
    try {
        const len = p.add(0x10).readS32();
        if (len <= 0 || len > 128) return '';
        return p.add(0x14).readUtf16String(Math.min(len, maxLen || 32)) || '';
    } catch (_) { return ''; }
}

function ptrToPat(p) {
    let v = uint64(p.toString());
    const bytes = [];
    for (let i = 0; i < 8; i++) {
        bytes.push(v.and(0xff).toString(16).padStart(2, '0'));
        v = v.shr(8);
    }
    return bytes.join(' ');
}

function findMetadataBase() {
    if (gMetaBase) return gMetaBase;
    for (const prot of ['r--', 'rw-']) {
        for (const r of Process.enumerateRanges(prot)) {
            if (r.size >= 0x12c0000 && r.size <= 0x1300000) {
                try {
                    if (r.base.add(4).readS32() === 24) {
                        gMetaBase = r.base;
                        return gMetaBase;
                    }
                } catch (_) {}
            }
        }
    }
    return null;
}

function scanClasses(force) {
    const needed = Object.keys(gClasses).filter(k => !gClasses[k]);
    if (needed.length === 0) return true;

    const now = Date.now();
    if (!force && now - gLastScanMs < CONFIG.rescanIntervalMs) return false;
    gLastScanMs = now;

    const meta = findMetadataBase();
    if (!meta) return false;
    const strOff = meta.add(24).readS32();
    const strSize = meta.add(28).readS32();
    const strBase = meta.add(strOff);
    const rwRanges = Process.enumerateRanges('rw-').filter(r => {
        if (r.size < 0x10000) return false;
        const u = uint64(r.base.toString()).and(PTR_TAG_MASK);
        return u.compare(PTR_MIN) >= 0 && u.compare(PTR_MAX) < 0;
    });

    const targets = [];
    for (const nm of needed) {
        const pat = nm.split('').map(c => c.charCodeAt(0).toString(16).padStart(2, '0')).join(' ') + ' 00';
        const matches = Memory.scanSync(strBase, strSize, pat).filter(m => {
            try { return m.address.sub(1).readU8() === 0; } catch (_) { return false; }
        });
        for (const m of matches) {
            targets.push({ nm, ppat: ptrToPat(m.address) });
        }
    }

    for (const r of rwRanges) {
        for (const t of targets) {
            if (gClasses[t.nm]) continue;
            try {
                const hits = Memory.scanSync(r.base, r.size, t.ppat);
                for (const h of hits) {
                    const k = h.address.sub(OFF.klass_name);
                    const nsP = safeReadPtr(k.add(OFF.klass_ns));
                    if (nsP.compare(strBase) >= 0 && nsP.compare(strBase.add(strSize)) < 0) {
                        if (isValidPtr(safeReadPtr(k))) {
                            const st = safeReadPtr(k.add(OFF.klass_static));
                            if (isValidPtr(st)) {
                                gClasses[t.nm] = k;
                                console.log('[mwr] Class ' + t.nm + ' @ ' + k + ' static=' + st);
                                break;
                            }
                        }
                    }
                }
            } catch (_) {}
        }
        if (needed.every(nm => gClasses[nm] !== null)) break;
    }
    return true;
}

function readState() {
    if (!gStatePtr) return null;
    return {
        aimbot: gStatePtr.add(ST.aimbot).readS32(),
        aimFovPx: gStatePtr.add(ST.aimFovPx).readFloat(),
        aimSmooth: gStatePtr.add(ST.aimSmooth).readFloat(),
        drawFov: gStatePtr.add(ST.drawFov).readS32(),
        aimTeamCheck: gStatePtr.add(ST.aimTeamCheck).readS32(),
        noRecoil: gStatePtr.add(ST.noRecoil).readS32(),
        noSpread: gStatePtr.add(ST.noSpread).readS32(),
        rapidFire: gStatePtr.add(ST.rapidFire).readS32(),
        espEnabled: gStatePtr.add(ST.espEnabled).readS32(),
        espTeamCheck: gStatePtr.add(ST.espTeamCheck).readS32(),
        espMaxDist: gStatePtr.add(ST.espMaxDist).readFloat(),
        speedHack: gStatePtr.add(ST.speedHack).readS32(),
        speedMult: gStatePtr.add(ST.speedMult).readFloat(),
        lowGravity: gStatePtr.add(ST.lowGravity).readS32(),
        gravityScale: gStatePtr.add(ST.gravityScale).readFloat(),
        acBypass: gStatePtr.add(ST.acBypass).readS32(),
        reqRescan: gStatePtr.add(ST.reqRescan).readS32(),
        reqReset: gStatePtr.add(ST.reqReset).readS32(),
        reqKill: gStatePtr.add(ST.reqKill).readS32(),
        aimVisCheck: gStatePtr.add(ST.aimVisCheck).readS32(),
        aimPriority: gStatePtr.add(ST.aimPriority).readS32(),
        aimTracer: gStatePtr.add(ST.aimTracer).readS32(),
    };
}

function restoreWeaponOriginals() {
    for (const k of Object.keys(orig.shootingMap)) {
        try {
            const o = orig.shootingMap[k];
            o.ptr.add(OFF.sh_fireDur).writeFloat(o.fireDur);
            o.ptr.add(OFF.sh_fireDurAuto).writeFloat(o.fireDurAuto);
        } catch (_) {}
    }
    orig.shootingMap = {};
    for (const k of Object.keys(orig.spreadMap)) {
        try {
            const o = orig.spreadMap[k];
            o.ptr.add(OFF.pw_accurateRatio).writeFloat(o.accRatio);
        } catch (_) {}
    }
    orig.spreadMap = {};
}

function restoreOriginals() {
    try {
        restoreWeaponOriginals();
        if (orig.acCaptured && gClasses.AntiCheatMgr) {
            const st = safeReadPtr(gClasses.AntiCheatMgr.add(OFF.klass_static));
            if (isValidPtr(st)) {
                st.add(OFF.ac_sceneWall).writeU8(orig.acBytes[0]);
                st.add(OFF.ac_teleport).writeU8(orig.acBytes[1]);
                st.add(OFF.ac_flyCheck).writeU8(orig.acBytes[2]);
                st.add(OFF.ac_block2Rep).writeU8(orig.acBytes[3]);
                st.add(OFF.ac_block2Reply).writeU8(orig.acBytes[4]);
                st.add(OFF.ac_block4Rep).writeU8(orig.acBytes[5]);
            }
        }
        if (gClasses.LocalPlayerManager) {
            const lpmSt = safeReadPtr(gClasses.LocalPlayerManager.add(OFF.klass_static));
            const lpm = safeReadPtr(lpmSt.add(OFF.lpm_instance));
            if (isValidPtr(lpm)) {
                if (orig.speedCaptured) {
                    lpm.add(OFF.lpm_debugSpeedMulti).writeFloat(orig.debugSpeedMulti);
                }
                const lp = safeReadPtr(lpm.add(OFF.lpm_localPlayer));
                if (isValidPtr(lp)) {
                    const pc = safeReadPtr(lp.add(OFF.pl_camera));
                    if (isValidPtr(pc) && orig.recoilCaptured) {
                        pc.add(OFF.pc_recoilEnabled).writeU8(orig.recoilEnabled);
                    }
                    const pm = safeReadPtr(lp.add(OFF.pl_movement));
                    if (isValidPtr(pm) && orig.gravCaptured) {
                        const gm = safeReadPtr(pm.add(OFF.pmov_gravityMod));
                        if (isValidPtr(gm)) {
                            gm.add(OFF.gm_gravityMul).writeFloat(orig.gravityMul);
                            gm.add(OFF.gm_fallSpeedMul).writeFloat(orig.fallSpeedMul);
                        }
                    }
                }
            }
        }
    } catch (_) {}
}

function resolvePlayerRenderer(share) {
    if (!isValidPtr(share)) return { natRend: ptr(0), av: ptr(0) };
    const av = safeReadPtr(share.add(OFF.aps_avatarCtrl));
    if (!isValidPtr(av)) return { natRend: ptr(0), av: ptr(0) };
    const mRend = safeReadPtr(av.add(OFF.av_mRenderer));
    const rDict = isValidPtr(mRend) ? safeReadPtr(mRend.add(OFF.ar_rendDict)) : ptr(0);
    const re = isValidPtr(rDict) ? safeReadPtr(rDict.add(OFF.dict_entries)) : ptr(0);
    if (!isValidPtr(re)) return { natRend: ptr(0), av };
    const arrLen = re.add(OFF.arr_len).readS32();
    const scan = Math.min(arrLen, 10);
    for (let j = 0; j < scan; j++) {
        const reE = re.add(OFF.arr_data + j * OFF.dictEntryStride);
        if (reE.readS32() < 0) continue;
        const lst = safeReadPtr(reE.add(OFF.dictEntryVal));
        if (!isValidPtr(lst)) continue;
        const sz = lst.add(OFF.list_size).readS32();
        if (sz <= 0) continue;
        const items = safeReadPtr(lst.add(OFF.list_items));
        if (!isValidPtr(items)) continue;
        const rend0 = safeReadPtr(items.add(OFF.arr_data));
        const natRend = isValidPtr(rend0) ? safeReadPtr(rend0.add(OFF.obj_cachedPtr)) : ptr(0);
        if (isValidPtr(natRend)) return { natRend, av };
    }
    return { natRend: ptr(0), av };
}

function writeEspSnapshot(nativeCamPtr, playerCamPtr, camTfPtr, recoilSpringPtr, entities) {
    if (!gEspPtr) return;
    const curSeq = gEspPtr.add(ESP.seq).readU32();
    const busySeq = (curSeq + 1) | 1;
    gEspPtr.add(ESP.seq).writeU32(busySeq);

    const cnt = Math.min(entities.length, ESP.MAX_ENT);
    gEspPtr.add(ESP.count).writeS32(cnt);
    gEspPtr.add(ESP.nativeCamPtr).writeU64(isValidPtr(nativeCamPtr) ? uint64(nativeCamPtr.toString()) : 0);
    gEspPtr.add(ESP.playerCameraPtr).writeU64(isValidPtr(playerCamPtr) ? uint64(playerCamPtr.toString()) : 0);
    gEspPtr.add(ESP.camTfPtr).writeU64(isValidPtr(camTfPtr) ? uint64(camTfPtr.toString()) : 0);
    gEspPtr.add(ESP.recoilSpringPtr).writeU64(isValidPtr(recoilSpringPtr) ? uint64(recoilSpringPtr.toString()) : 0);

    for (let i = 0; i < cnt; i++) {
        const e = entities[i];
        const base = gEspPtr.add(ESP.entities + i * ESP.entStride);
        base.add(ENT.nativeTfPtr).writeU64(isValidPtr(e.nativeTf) ? uint64(e.nativeTf.toString()) : 0);
        base.add(ENT.nativeRendPtr).writeU64(isValidPtr(e.nativeRend) ? uint64(e.nativeRend.toString()) : 0);
        base.add(ENT.x).writeFloat(e.x);
        base.add(ENT.y).writeFloat(e.y);
        base.add(ENT.z).writeFloat(e.z);
        base.add(ENT.hp).writeFloat(e.hp);
        base.add(ENT.maxHp).writeFloat(e.maxHp);
        base.add(ENT.isEnemy).writeS32(e.isEnemy ? 1 : 0);
        base.add(ENT.renderShow).writeU8(e.renderShow ? 1 : 0);
        base.add(ENT.name).writeUtf8String(e.name);
    }

    gEspPtr.add(ESP.seq).writeU32(busySeq + 1);
}

let lastTickErr = '';
let lastModActive = { ac: false, speed: false, grav: false, recoil: false, spread: false, rapid: false };

function tickFeatures() {
    if (gDestroyed || !gStatePtr || !gTelePtr) return CONFIG.idleTickMs;
    const st = readState();
    if (!st) return CONFIG.idleTickMs;

    if (st.reqKill) {
        gStatePtr.add(ST.reqKill).writeS32(0);
        send({ type: 'kill_menu' });
        return CONFIG.idleTickMs;
    }

    if (st.reqRescan) {
        gStatePtr.add(ST.reqRescan).writeS32(0);
        gClasses = { PlayerManager: null, LocalPlayerManager: null, CameraManager: null, AntiCheatMgr: null };
        gPlayerCache = {};
        gCachedLocalHeroKey = '';
        gCachedLocalTeamStr = '';
        scanClasses(true);
    }

    if (st.reqReset) {
        gStatePtr.add(ST.reqReset).writeS32(0);
        restoreOriginals();
    }

    if (!gClasses.PlayerManager || !gClasses.CameraManager || !gClasses.LocalPlayerManager) {
        scanClasses(false);
    }

    const menuOpen = fnIsMenuOpen ? (fnIsMenuOpen() !== 0) : false;
    const anyModOn = (st.espEnabled || st.aimbot || st.noRecoil || st.noSpread || st.rapidFire ||
                      st.speedHack || st.lowGravity || st.acBypass);
    let localPlayer = ptr(0);

    try {
        // 1. CameraManager (pointer only; C++ reads VP + camPos at 120Hz)
        let camMgrInst = ptr(0);
        let nativeCam = ptr(0);
        let natCamTf = ptr(0);

        if (gClasses.CameraManager) {
            const cmSt = safeReadPtr(gClasses.CameraManager.add(OFF.klass_static));
            camMgrInst = safeReadPtr(cmSt.add(OFF.cm_instance));
            if (isValidPtr(camMgrInst)) {
                const mCam = safeReadPtr(camMgrInst.add(OFF.cm_camera));
                if (isValidPtr(mCam)) nativeCam = safeReadPtr(mCam.add(OFF.obj_cachedPtr));
                const mCamTf = safeReadPtr(camMgrInst.add(OFF.cm_cameraTf));
                if (isValidPtr(mCamTf)) natCamTf = safeReadPtr(mCamTf.add(OFF.obj_cachedPtr));
            }
        }

        // 2. LocalPlayerManager & PlayerManager
        let lpmInst = ptr(0);
        let playerCam = ptr(0);
        let recoilSpring = ptr(0);
        let localHero = ptr(0);
        let localNetId = -1;
        let localHp = 0, localMaxHp = 0;

        if (gClasses.LocalPlayerManager) {
            const lpmSt = safeReadPtr(gClasses.LocalPlayerManager.add(OFF.klass_static));
            lpmInst = safeReadPtr(lpmSt.add(OFF.lpm_instance));
            if (isValidPtr(lpmInst)) {
                localPlayer = safeReadPtr(lpmInst.add(OFF.lpm_localPlayer));
                localNetId = lpmInst.add(OFF.lpm_bindNetId).readS32();
                if (isValidPtr(localPlayer)) {
                    playerCam = safeReadPtr(localPlayer.add(OFF.pl_camera));
                    if (isValidPtr(playerCam)) {
                        const cph = safeReadPtr(playerCam.add(OFF.pc_physHandler));
                        if (isValidPtr(cph)) recoilSpring = safeReadPtr(cph.add(OFF.cph_recoilSpring));
                    }
                }
            }
        }

        const needEntities = (st.espEnabled || st.aimbot || menuOpen);
        let pmStatic = ptr(0);
        let dictCount = 0;
        let enemyCount = 0;
        const entities = [];

        if (gClasses.PlayerManager) {
            pmStatic = safeReadPtr(gClasses.PlayerManager.add(OFF.klass_static));
            if (isValidPtr(pmStatic)) {
                localHero = safeReadPtr(pmStatic.add(OFF.pm_localHero));
                if (isValidPtr(localHero)) {
                    const lhKey = localHero.toString();
                    const hShare = safeReadPtr(localHero.add(OFF.np_shareData));
                    const hNpd = isValidPtr(hShare) ? safeReadPtr(hShare.add(OFF.ahs_netData)) : ptr(0);
                    if (isValidPtr(hNpd)) {
                        if (lhKey !== gCachedLocalHeroKey || !gCachedLocalTeamStr) {
                            gCachedLocalHeroKey = lhKey;
                            gCachedLocalTeamStr = readIl2CppString(safeReadPtr(hNpd.add(OFF.npd_teamIdStr)), 32);
                            gPlayerCache = {};
                        }
                        if (menuOpen) {
                            const bat = safeReadPtr(hNpd.add(OFF.npd_battle));
                            if (isValidPtr(bat)) {
                                const hpV = safeReadPtr(bat.add(OFF.bat_hp));
                                const mhpV = safeReadPtr(bat.add(OFF.bat_maxHp));
                                if (isValidPtr(hpV)) localHp = hpV.add(OFF.nv_internalValue).readS32();
                                if (isValidPtr(mhpV)) localMaxHp = mhpV.add(OFF.nv_internalValue).readS32();
                            }
                        }
                    }
                } else if (gCachedLocalHeroKey !== '') {
                    gCachedLocalHeroKey = '';
                    gCachedLocalTeamStr = '';
                    gPlayerCache = {};
                }

                const dict = safeReadPtr(pmStatic.add(OFF.pm_playerDict));
                if (isValidPtr(dict)) {
                    dictCount = dict.add(OFF.dict_count).readS32();
                    if (needEntities && dictCount > 0 && dictCount < 256) {
                        const entries = safeReadPtr(dict.add(OFF.dict_entries));
                        if (isValidPtr(entries)) {
                            const arrLen = entries.add(OFF.arr_len).readS32();
                            const scanLen = Math.min(arrLen, 200);
                            for (let i = 0; i < scanLen; i++) {
                                const entAddr = entries.add(OFF.arr_data + i * OFF.dictEntryStride);
                                if (entAddr.readS32() < 0) continue;
                                const netPlayer = safeReadPtr(entAddr.add(OFF.dictEntryVal));
                                if (!isValidPtr(netPlayer)) continue;
                                if (isValidPtr(localHero) && netPlayer.equals(localHero)) continue;
                                if (netPlayer.add(OFF.np_isDestroy).readU8() !== 0) continue;

                                const keyStr = netPlayer.toString();
                                let cached = gPlayerCache[keyStr];
                                const share = safeReadPtr(netPlayer.add(OFF.np_shareData));
                                const npd = isValidPtr(share) ? safeReadPtr(share.add(OFF.aps_netData)) : ptr(0);

                                if (!cached) {
                                    let netId = isValidPtr(share) ? share.add(OFF.aps_netPlayerId).readS32() : -1;
                                    let teamStr = '';
                                    let pName = '';
                                    if (isValidPtr(npd)) {
                                        teamStr = readIl2CppString(safeReadPtr(npd.add(OFF.npd_teamIdStr)), 32);
                                        pName = sanitizeLabel(readIl2CppString(safeReadPtr(npd.add(OFF.npd_playerNameStr)), 32), netId);
                                    } else {
                                        pName = sanitizeLabel('', netId);
                                    }
                                    const rInfo = resolvePlayerRenderer(share);
                                    cached = { netId, teamStr, name: pName, natRend: rInfo.natRend, av: rInfo.av };
                                    gPlayerCache[keyStr] = cached;
                                } else if (!isValidPtr(cached.natRend)) {
                                    const rInfo = resolvePlayerRenderer(share);
                                    cached.natRend = rInfo.natRend;
                                    cached.av = rInfo.av;
                                }

                                const renderShow = isValidPtr(cached.av) ? cached.av.add(OFF.av_renderShow).readU8() : 1;

                                if (localNetId > 0 && cached.netId === localNetId) continue;

                                let mTf = safeReadPtr(netPlayer.add(OFF.np_transform));
                                if (!isValidPtr(mTf) && isValidPtr(share)) {
                                    mTf = safeReadPtr(share.add(OFF.aps_playerTrans));
                                }
                                const natTf = isValidPtr(mTf) ? safeReadPtr(mTf.add(OFF.obj_cachedPtr)) : ptr(0);

                                let hp = 100, maxHp = 100;
                                let fx = 0, fy = 0, fz = 0;
                                if (isValidPtr(npd)) {
                                    const bat = safeReadPtr(npd.add(OFF.npd_battle));
                                    if (isValidPtr(bat)) {
                                        const hpV = safeReadPtr(bat.add(OFF.bat_hp));
                                        const mhpV = safeReadPtr(bat.add(OFF.bat_maxHp));
                                        if (isValidPtr(hpV)) hp = hpV.add(OFF.nv_internalValue).readS32();
                                        if (isValidPtr(mhpV)) maxHp = mhpV.add(OFF.nv_internalValue).readS32();
                                    }
                                    if (!isValidPtr(natTf)) {
                                        const pv = safeReadPtr(npd.add(OFF.npd_playerPos));
                                        if (isValidPtr(pv)) {
                                            fx = pv.add(OFF.nv_internalValue).readFloat();
                                            fy = pv.add(OFF.nv_internalValue + 4).readFloat();
                                            fz = pv.add(OFF.nv_internalValue + 8).readFloat();
                                        }
                                    }
                                }

                                const isEnemy = (gCachedLocalTeamStr && cached.teamStr)
                                    ? (cached.teamStr !== gCachedLocalTeamStr)
                                    : true;
                                if (isEnemy) enemyCount++;

                                entities.push({
                                    nativeTf: natTf,
                                    nativeRend: cached.natRend,
                                    x: fx,
                                    y: fy,
                                    z: fz,
                                    hp: hp,
                                    maxHp: maxHp,
                                    isEnemy: isEnemy,
                                    renderShow: renderShow,
                                    name: cached.name,
                                });
                            }
                        }
                    }
                }
            }
        }

        if (st.espEnabled || st.aimbot || st.noRecoil) {
            writeEspSnapshot(nativeCam, playerCam, natCamTf, recoilSpring, entities);
        }

        // 3. AntiCheatMgr telemetry disable
        if (gClasses.AntiCheatMgr) {
            const acSt = safeReadPtr(gClasses.AntiCheatMgr.add(OFF.klass_static));
            if (isValidPtr(acSt)) {
                if (!orig.acCaptured) {
                    orig.acBytes = [
                        acSt.add(OFF.ac_sceneWall).readU8(),
                        acSt.add(OFF.ac_teleport).readU8(),
                        acSt.add(OFF.ac_flyCheck).readU8(),
                        acSt.add(OFF.ac_block2Rep).readU8(),
                        acSt.add(OFF.ac_block2Reply).readU8(),
                        acSt.add(OFF.ac_block4Rep).readU8(),
                    ];
                    orig.acCaptured = true;
                }
                if (st.acBypass) {
                    acSt.add(OFF.ac_sceneWall).writeU8(0);
                    acSt.add(OFF.ac_teleport).writeU8(0);
                    acSt.add(OFF.ac_flyCheck).writeU8(0);
                    acSt.add(OFF.ac_block2Rep).writeU8(1);
                    acSt.add(OFF.ac_block2Reply).writeU8(1);
                    acSt.add(OFF.ac_block4Rep).writeU8(1);
                    lastModActive.ac = true;
                } else if (lastModActive.ac) {
                    acSt.add(OFF.ac_sceneWall).writeU8(orig.acBytes[0]);
                    acSt.add(OFF.ac_teleport).writeU8(orig.acBytes[1]);
                    acSt.add(OFF.ac_flyCheck).writeU8(orig.acBytes[2]);
                    acSt.add(OFF.ac_block2Rep).writeU8(orig.acBytes[3]);
                    acSt.add(OFF.ac_block2Reply).writeU8(orig.acBytes[4]);
                    acSt.add(OFF.ac_block4Rep).writeU8(orig.acBytes[5]);
                    lastModActive.ac = false;
                }
            }
        }

        // 4. LocalPlayer combat & movement
        if (isValidPtr(lpmInst)) {
            if (!orig.speedCaptured) {
                orig.debugSpeedMulti = lpmInst.add(OFF.lpm_debugSpeedMulti).readFloat();
                if (orig.debugSpeedMulti <= 0.01) orig.debugSpeedMulti = 1.0;
                orig.speedCaptured = true;
            }
            if (st.speedHack) {
                lpmInst.add(OFF.lpm_debugSpeedMulti).writeFloat(orig.debugSpeedMulti * st.speedMult);
                lastModActive.speed = true;
            } else if (lastModActive.speed) {
                lpmInst.add(OFF.lpm_debugSpeedMulti).writeFloat(orig.debugSpeedMulti);
                lastModActive.speed = false;
            }
        }

        if (isValidPtr(localPlayer)) {
            if (isValidPtr(playerCam)) {
                if (!orig.recoilCaptured) {
                    orig.recoilEnabled = playerCam.add(OFF.pc_recoilEnabled).readU8();
                    orig.recoilCaptured = true;
                }
                if (st.noRecoil) {
                    playerCam.add(OFF.pc_recoilEnabled).writeU8(0);
                    playerCam.add(OFF.pc_recoilAngles).writeFloat(0.0);
                    playerCam.add(OFF.pc_recoilAngles + 4).writeFloat(0.0);
                    lastModActive.recoil = true;
                } else if (lastModActive.recoil) {
                    playerCam.add(OFF.pc_recoilEnabled).writeU8(orig.recoilEnabled);
                    lastModActive.recoil = false;
                }
            }

            if (st.lowGravity || lastModActive.grav) {
                const pm = safeReadPtr(localPlayer.add(OFF.pl_movement));
                if (isValidPtr(pm)) {
                    const gm = safeReadPtr(pm.add(OFF.pmov_gravityMod));
                    if (isValidPtr(gm)) {
                        if (!orig.gravCaptured) {
                            orig.gravityMul = gm.add(OFF.gm_gravityMul).readFloat();
                            orig.fallSpeedMul = gm.add(OFF.gm_fallSpeedMul).readFloat();
                            orig.gravCaptured = true;
                        }
                        if (st.lowGravity) {
                            gm.add(OFF.gm_gravityMul).writeFloat(orig.gravityMul * st.gravityScale);
                            gm.add(OFF.gm_fallSpeedMul).writeFloat(orig.fallSpeedMul * st.gravityScale);
                            lastModActive.grav = true;
                        } else if (lastModActive.grav) {
                            gm.add(OFF.gm_gravityMul).writeFloat(orig.gravityMul);
                            gm.add(OFF.gm_fallSpeedMul).writeFloat(orig.fallSpeedMul);
                            lastModActive.grav = false;
                        }
                    }
                }
            }

            if (st.noSpread || st.rapidFire || lastModActive.spread || lastModActive.rapid) {
                const eqVal = safeReadPtr(localPlayer.add(OFF.pl_equippedItemVal));
                const gun = isValidPtr(eqVal) ? safeReadPtr(eqVal.add(OFF.val_currentValue)) : ptr(0);
                if (isValidPtr(gun)) {
                    const gunKey = gun.toString();
                    if (st.noSpread) {
                        if (!orig.spreadMap[gunKey]) {
                            orig.spreadMap[gunKey] = {
                                ptr: gun,
                                accRatio: gun.add(OFF.pw_accurateRatio).readFloat(),
                            };
                        }
                        gun.add(OFF.pw_curAccurate).writeFloat(0.0);
                        gun.add(OFF.pw_shootAccurate).writeFloat(0.0);
                        gun.add(OFF.pw_accurateRatio).writeFloat(0.0);
                        lastModActive.spread = true;
                    } else if (lastModActive.spread) {
                        for (const k of Object.keys(orig.spreadMap)) {
                            try {
                                const o = orig.spreadMap[k];
                                o.ptr.add(OFF.pw_accurateRatio).writeFloat(o.accRatio);
                            } catch (_) {}
                        }
                        orig.spreadMap = {};
                        lastModActive.spread = false;
                    }

                    if (st.rapidFire || lastModActive.rapid) {
                        const mPW = safeReadPtr(gun.add(OFF.pw_mPW));
                        const sh = isValidPtr(mPW) ? safeReadPtr(mPW.add(OFF.pwi_shooting)) : ptr(0);
                        if (st.rapidFire) {
                            gun.add(OFF.eq_nextUseTime).writeFloat(0.0);
                            gun.add(OFF.pw_fireDurTimer).writeFloat(0.0);
                            if (isValidPtr(sh)) {
                                const shKey = sh.toString();
                                if (!orig.shootingMap[shKey]) {
                                    orig.shootingMap[shKey] = {
                                        ptr: sh,
                                        fireDur: sh.add(OFF.sh_fireDur).readFloat(),
                                        fireDurAuto: sh.add(OFF.sh_fireDurAuto).readFloat(),
                                    };
                                }
                                sh.add(OFF.sh_fireDur).writeFloat(0.025);
                                sh.add(OFF.sh_fireDurAuto).writeFloat(0.045);
                            }
                            lastModActive.rapid = true;
                        } else if (lastModActive.rapid) {
                            for (const k of Object.keys(orig.shootingMap)) {
                                try {
                                    const o = orig.shootingMap[k];
                                    o.ptr.add(OFF.sh_fireDur).writeFloat(o.fireDur);
                                    o.ptr.add(OFF.sh_fireDurAuto).writeFloat(o.fireDurAuto);
                                } catch (_) {}
                            }
                            orig.shootingMap = {};
                            lastModActive.rapid = false;
                        }
                    }
                }
            }
        }

        // 5. Update Telemetry struct (only full update when menu is open or status changed)
        gTelePtr.add(TELE.playerMgrAddr).writeU64(isValidPtr(pmStatic) ? uint64(pmStatic.toString()) : 0);
        gTelePtr.add(TELE.localHeroAddr).writeU64(isValidPtr(localHero) ? uint64(localHero.toString()) : 0);
        gTelePtr.add(TELE.localPlayerAddr).writeU64(isValidPtr(localPlayer) ? uint64(localPlayer.toString()) : 0);
        gTelePtr.add(TELE.camMgrAddr).writeU64(isValidPtr(camMgrInst) ? uint64(camMgrInst.toString()) : 0);
        gTelePtr.add(TELE.nativeCamAddr).writeU64(isValidPtr(nativeCam) ? uint64(nativeCam.toString()) : 0);
        if (menuOpen) {
            gTelePtr.add(TELE.playerCount).writeS32(dictCount);
            gTelePtr.add(TELE.enemyCount).writeS32(enemyCount);
            gTelePtr.add(TELE.localHp).writeFloat(localHp);
            gTelePtr.add(TELE.localMaxHp).writeFloat(localMaxHp);
            gTelePtr.add(TELE.localTeamId).writeS32(localNetId);
            gTelePtr.add(TELE.statusText).writeUtf8String(
                isValidPtr(localPlayer) ? 'Active in match' : (isValidPtr(camMgrInst) ? 'Waiting for match' : 'Scanning IL2CPP...')
            );
        }
    } catch (e) {
        const msg = String(e);
        if (msg !== lastTickErr) {
            lastTickErr = msg;
            console.log('[mwr-tick-err] ' + msg);
        }
    }

    return (isValidPtr(localPlayer) && anyModOn) ? CONFIG.activeTickMs : CONFIG.idleTickMs;
}

function scheduleNextTick() {
    if (gDestroyed) return;
    const delay = tickFeatures();
    if (!gDestroyed) {
        gTickTimer = setTimeout(scheduleNextTick, delay);
    }
}

function initImGuiBridge() {
    gImGuiMod = Module.load(CONFIG.soPath);
    console.log('[mwr] Loaded ' + CONFIG.soPath + ' @ ' + gImGuiMod.base);

    const fnGetState = new NativeFunction(gImGuiMod.findExportByName('imgui_get_state'), 'pointer', []);
    const fnGetTele = new NativeFunction(gImGuiMod.findExportByName('imgui_get_telemetry'), 'pointer', []);
    const fnGetEsp = new NativeFunction(gImGuiMod.findExportByName('imgui_get_esp_snapshot'), 'pointer', []);
    fnOnSwap = new NativeFunction(gImGuiMod.findExportByName('imgui_on_swap'), 'void', ['pointer', 'pointer']);
    fnOnTouch = new NativeFunction(gImGuiMod.findExportByName('imgui_on_touch'), 'int', ['int', 'int', 'float', 'float']);
    fnReqShutdown = new NativeFunction(gImGuiMod.findExportByName('imgui_request_shutdown'), 'void', []);
    fnIsShutdownDone = new NativeFunction(gImGuiMod.findExportByName('imgui_is_shutdown_done'), 'int', []);
    fnToggleMenu = new NativeFunction(gImGuiMod.findExportByName('imgui_toggle_menu'), 'void', []);
    fnSetMenuOpen = new NativeFunction(gImGuiMod.findExportByName('imgui_set_menu_open'), 'void', ['int']);
    fnIsMenuOpen = new NativeFunction(gImGuiMod.findExportByName('imgui_is_menu_open'), 'int', []);
    fnGetSurfaceSize = new NativeFunction(gImGuiMod.findExportByName('imgui_get_surface_size'), 'void', ['pointer', 'pointer']);

    const fnResetLifecycle = new NativeFunction(gImGuiMod.findExportByName('imgui_reset_lifecycle'), 'void', []);
    fnResetLifecycle();

    gStatePtr = fnGetState();
    gTelePtr = fnGetTele();
    gEspPtr = fnGetEsp();
    gSurfSizeBuf = Memory.alloc(8);

    const egl = Process.findModuleByName('libEGL.so');
    if (!egl) throw new Error('libEGL.so not found');
    const swapAddr = egl.findExportByName('eglSwapBuffers');
    if (!swapAddr) throw new Error('eglSwapBuffers not found');

    gSwapHook = Interceptor.attach(swapAddr, {
        onEnter(args) {
            if (!gSwapDetaching) {
                fnOnSwap(args[0], args[1]);
            }
        }
    });
    console.log('[mwr] Hooked eglSwapBuffers @ ' + swapAddr);

    Java.perform(function () {
        const Activity = Java.use('android.app.Activity');
        Activity.dispatchTouchEvent.implementation = function (ev) {
            if (!gDestroyed && fnOnTouch && ev) {
                try {
                    const actMasked = ev.getActionMasked();
                    let scaleX = 1.0, scaleY = 1.0;
                    const win = this.getWindow();
                    if (win) {
                        const decor = win.getDecorView();
                        const vw = decor.getWidth();
                        const vh = decor.getHeight();
                        if (vw > 0 && vh > 0) {
                            fnGetSurfaceSize(gSurfSizeBuf, gSurfSizeBuf.add(4));
                            const sw = gSurfSizeBuf.readS32();
                            const sh = gSurfSizeBuf.add(4).readS32();
                            if (sw > 0 && sh > 0) {
                                scaleX = sw / vw;
                                scaleY = sh / vh;
                            }
                        }
                    }

                    if (actMasked === 2) {
                        const count = ev.getPointerCount();
                        let consumedCount = 0;
                        for (let i = 0; i < count; i++) {
                            const pid = ev.getPointerId(i);
                            const sx = ev.getX(i) * scaleX;
                            const sy = ev.getY(i) * scaleY;
                            if (fnOnTouch(2, pid, sx, sy) !== 0) {
                                consumedCount++;
                            }
                        }
                        if (consumedCount === count && count > 0) {
                            return true;
                        }
                    } else {
                        const idx = ev.getActionIndex();
                        const pid = ev.getPointerId(idx);
                        const sx = ev.getX(idx) * scaleX;
                        const sy = ev.getY(idx) * scaleY;
                        if (fnOnTouch(actMasked, pid, sx, sy) !== 0) {
                            if (ev.getPointerCount() <= 1 || actMasked === 0 || actMasked === 5) {
                                return true;
                            }
                        }
                    }
                } catch (_) {}
            }
            return this.dispatchTouchEvent(ev);
        };
        gTouchHooked = true;
        console.log('[mwr] Hooked Activity.dispatchTouchEvent for Dear ImGui.');
    });
}

function cleanup() {
    if (gDestroyed) return 'already cleaned';
    gDestroyed = true;

    if (gTickTimer) { clearTimeout(gTickTimer); gTickTimer = null; }

    if (gTouchHooked) {
        try {
            Java.perform(function () {
                const Activity = Java.use('android.app.Activity');
                Activity.dispatchTouchEvent.implementation = null;
            });
        } catch (_) {}
        gTouchHooked = false;
    }

    if (fnReqShutdown && fnIsShutdownDone) {
        try {
            fnReqShutdown();
            const deadline = Date.now() + 500;
            while (fnIsShutdownDone() === 0 && Date.now() < deadline) {
                Thread.sleep(0.01);
            }
        } catch (_) {}
    }

    gSwapDetaching = true;
    if (gSwapHook) {
        try { gSwapHook.detach(); } catch (_) {}
        gSwapHook = null;
    }

    restoreOriginals();
    return 'cleaned';
}

rpc.exports = {
    cleanup: cleanup,
    toggleMenu: function () {
        if (fnToggleMenu) fnToggleMenu();
        return 'toggled';
    },
    setMenuOpen: function (open) {
        if (fnSetMenuOpen) fnSetMenuOpen(open ? 1 : 0);
        return open ? 'open' : 'closed';
    },
    enableTestFeatures: function () {
        if (!gStatePtr) return 'no state';
        gStatePtr.add(ST.espEnabled).writeS32(1);
        gStatePtr.add(ST.aimbot).writeS32(1);
        gStatePtr.add(ST.noRecoil).writeS32(1);
        gStatePtr.add(ST.noSpread).writeS32(1);
        gStatePtr.add(ST.rapidFire).writeS32(1);
        return 'enabled';
    },
    status: function () {
        return {
            classes: {
                PlayerManager: gClasses.PlayerManager ? gClasses.PlayerManager.toString() : null,
                LocalPlayerManager: gClasses.LocalPlayerManager ? gClasses.LocalPlayerManager.toString() : null,
                CameraManager: gClasses.CameraManager ? gClasses.CameraManager.toString() : null,
                AntiCheatMgr: gClasses.AntiCheatMgr ? gClasses.AntiCheatMgr.toString() : null,
            },
            imguiBase: gImGuiMod ? gImGuiMod.base.toString() : null,
            state: gStatePtr ? readState() : null,
        };
    },
};

let gStarted = false;

function startMod() {
    if (gStarted) return;
    gStarted = true;
    console.log('[mwr] Initializing Mini World Royale Dear ImGui Mod Menu...');
    scanClasses(true);
    initImGuiBridge();
    scheduleNextTick();
}

function stageSoFromBytes(targetPath, bytes) {
    const f = new File(targetPath, 'wb');
    f.write(bytes);
    f.flush();
    f.close();
    const chmodSym = (typeof Module.findGlobalExportByName === 'function')
        ? Module.findGlobalExportByName('chmod')
        : Module.findExportByName(null, 'chmod');
    if (chmodSym) {
        const fnChmod = new NativeFunction(chmodSym, 'int', ['pointer', 'int']);
        fnChmod(Memory.allocUtf8String(targetPath), 0o555);
    }
    CONFIG.soPath = targetPath;
    console.log('[mwr] Staged .so via Frida inside app context: ' + targetPath);
}

setImmediate(function () {
    if (CONFIG.waitForStage) {
        console.log('[mwr] Waiting for in-process .so stage payload...');
        recv('stage_so', function (msg, data) {
            try {
                stageSoFromBytes(msg.path || CONFIG.soPath, data);
                startMod();
            } catch (e) {
                console.log('[mwr] stage_so failed: ' + e);
            }
        });
    } else {
        startMod();
    }
});

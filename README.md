# Mini World Royale — Mod Menu & External Overlay (`mwr_mod_menu`)

Bộ công cụ mod & ESP/Aimbot dùng Dear ImGui + OpenGL ES 3 cho **Mini World Royale** (`com.vn.miniworld.royale.sparkgame` / `com.sea.miniworldroyale.sparkgame`) trên Android (ARM64 & x86_64), tích hợp sẵn **Auto-Resolve IL2CPP** qua `/proc/<pid>/mem`.

---

## 1. Các chế độ hoạt động

| Chế độ | File chạy | Cơ chế | Ghi chú |
|---|---|---|---|
| **External Overlay (Khuyên dùng)** | `build/mwr_ext.sh` | Process riêng ngoài game, render qua SurfaceFlinger (`ANativeWindowCreator`), đọc touch `/dev/input/event*`, đọc RAM qua `pread64` (`/proc/<pid>/mem`) | Không inject `.so`, không hook `eglSwapBuffers` / GOT / code game |
| **Internal Standalone (Root)** | `build/mwr_mod.sh` | Inject `libmwr_imgui.so` (ARM64) bằng `ptrace` (`mwr_inject`), hook GOT `eglSwapBuffers` & `InputConsumer::consume` | Có thêm mod can thiệp gameplay trực tiếp & nút `Unload` |
| **PC + Frida (Debug / Emulator)** | `launch.py` | Đẩy `libmwr_imgui.so` + điều khiển qua `frida-server` (`mwr_mod.js`) | Dùng với MuMu x86_64 hoặc máy thật chạy `frida-server` |

---

## 2. Tính năng

### External Overlay (`mwr_ext.sh` — nút kéo thả `MWR EXT`)
- **Combat**:
  - `Aimbot (LookAngles)` — xoay góc nhìn qua `PlayerCamera.m_LookAngles` (bù `m_RecoilAngles`), chỉ ghi khi bật Aimbot và có target lock
  - `FOV radius (px)`, `Smoothing`, `Priority` (`Near FOV` / `Closest` / `Low HP`), `Bone` (`Head` / `Chest` / `Body`)
  - `Visibility Check` (đọc cờ frustum/render visibility native của `UnityEngine::Renderer`), `Target Tracer`, `Draw FOV Circle`, `Hide Teammates (Aim)`
- **Visuals**:
  - `Enable Player ESP`, `2D Bounding Box`, `Snaplines`, `Health Bar`, `Name & Distance`, `Hide Teammates`, `Visibility Check`, `Max Dist (m)`
- **System**:
  - FPS / độ phân giải, trạng thái `Read-Only` / `Aim RW`, địa chỉ `NativeCam` / `PlayerCam`, `Players` / `Enemies`, HP & Team local, nút `Rescan IL2CPP` và `Exit Overlay`.

### Internal Mod Menu (`mwr_mod.sh` — nút `MWR`)
- Đầy đủ ESP & Aimbot như trên + `No Recoil`, `No Spread`, `Rapid Fire`, `Speed Hack`, `Low Gravity`, `AC Bypass`, nút `Unload` gỡ hook sạch.

---

## 3. Chạy nhanh trên điện thoại Root (ARM64)

Tải sẵn file trong [`build/`](build/) hoặc mục [Releases](../../releases):

### External Overlay (`mwr_ext.sh`)
```powershell
adb push build\mwr_ext.sh /data/local/tmp/mwr_ext.sh
adb shell chmod 755 /data/local/tmp/mwr_ext.sh
```
Chạy với quyền root (ADB `su` / KernelSU / Termux `su` / MT Manager):
```sh
sh /data/local/tmp/mwr_ext.sh
```
Script tự kiểm tra `com.vn.miniworld.royale.sparkgame` / `com.sea.miniworldroyale.sparkgame` (hoặc truyền package name làm tham số đầu), tự mở game nếu chưa chạy, giải nén binary `mwr_ext` vào `/data/local/tmp/.mwr_ext_<pid>` rồi dọn sạch khi thoát.

### Internal Injector (`mwr_mod.sh`)
```powershell
adb push build\mwr_mod.sh /data/local/tmp/mwr_mod.sh
adb shell chmod 755 /data/local/tmp/mwr_mod.sh
```
```sh
sh /data/local/tmp/mwr_mod.sh
```
Tự đợi `libil2cpp.so` + `libunity.so` + `libEGL.so`, stage `libmwr_imgui.so` đúng UID & SELinux context vào `/data/data/<pkg>/cache/`, rồi chạy `mwr_inject`.

### PC + Frida
```powershell
python launch.py
```

---

## 4. Tự build từ source (Windows + Android NDK)

1. Yêu cầu:
   - Android NDK r28 (`28.2.13676358`, chỉnh biến `$ndk` trong [`build.ps1`](build.ps1) nếu cài ở đường dẫn khác)
   - Python 3 (để chạy `pack_sh.py` & `pack_ext_sh.py`)
2. Biên dịch toàn bộ binary (`arm64-v8a`, `x86_64`) và đóng gói `build/mwr_mod.sh` + `build/mwr_ext.sh`:
```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

> Lưu ý khi clone trên Windows: repo đã cấu hình [`.gitattributes`](.gitattributes) giữ line ending `LF` cho `*.sh` để các script self-extract chạy được trực tiếp trên Android.

---

## 5. Cấu trúc thư mục

```text
mwr_mod_menu/
├── build.ps1                    # Script compile NDK (arm64-v8a + x86_64) & pack .sh
├── pack_ext_sh.py               # Đóng gói build/mwr_ext.sh
├── pack_sh.py                   # Đóng gói build/mwr_mod.sh
├── launch.py / mwr_mod.js       # Launcher Frida cho emulator / root PC
├── mwr_start.sh                 # Helper script khởi chạy
├── imgui/                       # Dear ImGui (OpenGL ES 3 backend)
├── src/
│   ├── mwr_ext_main.cpp         # External overlay + Auto-resolve IL2CPP + ESP/Aim
│   ├── mwr_standalone.cpp       # Internal IL2CPP engine + hooks
│   ├── mwr_imgui.cpp            # Render/Input EGL & shared state
│   ├── mwr_inject.cpp           # ptrace __loader_dlopen injector (ARM64)
│   ├── mwr_shared.h / mwr_ui.h  # Struct dùng chung & giao diện ImGui internal
│   └── external/
│       ├── ANativeWindowCreator.h  # SurfaceFlinger native overlay window
│       ├── ExtMemReader.h          # /proc/<pid>/mem pread64/pwrite64 wrapper
│       └── ExtTouch.h/.cpp         # /dev/input/event* multitouch reader
└── build/
    ├── mwr_ext.sh               # All-in-one External root script
    ├── mwr_mod.sh               # All-in-one Internal root script
    ├── arm64-v8a/               # mwr_ext, mwr_inject, libmwr_imgui.so (stripped)
    └── x86_64/                  # libmwr_imgui.so (emulator)
```

---

## 6. Khả năng sống qua Game Update

- **Tự cập nhật qua patch thường**: tìm `global-metadata` trong RAM theo header version (`21..35`) + chữ ký `mscorlib`, quét 13 `Il2CppClass` (`PlayerManager`, `LocalPlayerManager`, `CameraManager`, `NetPlayerBase`, `Hero`, `NetPlayerData2`, `Battle`, `Player`, `PlayerCamera`, `ActorPlayerShareData`, `ActorHeroShareData`, `AvatarRendererComp`, `Object`), tự resolve toàn bộ offset `FieldInfo` C# theo tên, tự probe `klass_fields` (`0x70..0xa0`) và `Camera` VP (`0xc0..0x130`).
- **Trường hợp cần update lại code**: game đổi tên/obfuscate class & field C#, nâng major version Unity làm dịch struct C++ (`Renderer`, `TransformAccess`, `Il2CppClass`, `Array`, `Dictionary`), hoặc mã hóa `global-metadata` trong RAM.

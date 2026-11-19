# Mini World Royale — Dear ImGui Mod Menu (`mwr_mod_menu`)

## Cách 1: Chạy trực tiếp 1 file `.sh` trên điện thoại (Standalone Root — không cần PC / Frida)
1. Build bản mới nhất (tạo luôn file gộp `build\mwr_mod.sh`):
   ```powershell
   powershell -ExecutionPolicy Bypass -File G:\hacker\gg\mwr_mod_menu\build.ps1
   ```
2. Push 1 file duy nhất vào `/data/local/tmp/`:
   ```powershell
   adb push G:\hacker\gg\mwr_mod_menu\build\mwr_mod.sh /data/local/tmp/mwr_mod.sh
   adb shell chmod 755 /data/local/tmp/mwr_mod.sh
   ```
3. Trên điện thoại (MT Manager / Termux `su` / KernelSU), cấp quyền **root** và chạy:
   ```sh
   sh /data/local/tmp/mwr_mod.sh
   ```
   - Tự giải nén `mwr_inject` + `libmwr_imgui.so` (ARM64) ra thư mục tạm rồi dọn sạch sau khi tiêm.
   - Tự nhận diện game (`com.vn.miniworld.royale.sparkgame` hoặc `com.sea.miniworldroyale.sparkgame`), tự mở game nếu chưa chạy, đợi `libil2cpp.so` + `libunity.so` + `libEGL.so` load.
   - Tự stage `.so` đúng UID + SELinux MCS context vào `/data/data/<pkg>/cache/` và inject (`ptrace` `__loader_dlopen`).
   - Tự hook `libunity.so` (`eglSwapBuffers`), `libandroid_runtime.so` (`InputConsumer::consume`), và chạy IL2CPP engine ngay trong C++.

## Cách 2: Chạy qua PC + Frida (MuMu hoặc điện thoại chạy `frida-server`)
```powershell
python G:\hacker\gg\mwr_mod_menu\launch.py
```
- Nếu máy thật chạy `frida-server 17.5.2`:
  ```powershell
  G:\hacker\gg\mwr_mod_menu\.venv_phone\Scripts\python.exe G:\hacker\gg\mwr_mod_menu\launch.py
  ```

## Gỡ menu an toàn (Clean Unload)
- Bấm **`Unload`** trong tab **System**: khôi phục toàn bộ giá trị game, khôi phục GOT hook (`eglSwapBuffers`, `InputConsumer::consume`) và tắt menu sạch sẽ.

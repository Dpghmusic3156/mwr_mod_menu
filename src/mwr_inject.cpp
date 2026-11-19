#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/ptrace.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <dlfcn.h>
#include <string>
#include <vector>

#ifndef __aarch64__
#error "mwr_inject is designed for arm64-v8a (aarch64)"
#endif

static bool FindRemoteModule(pid_t pid, const char* substr, uint64_t& outBase, std::string& outPath) {
    char mapsPath[64];
    snprintf(mapsPath, sizeof(mapsPath), "/proc/%d/maps", pid);
    FILE* f = fopen(mapsPath, "r");
    if (!f) return false;

    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char* path = strchr(line, '/');
        if (!path || !strstr(path, substr)) continue;

        uint64_t start = 0, end = 0, offset = 0;
        char perms[8] = {};
        if (sscanf(line, "%lx-%lx %7s %lx", &start, &end, perms, &offset) == 4) {
            if (offset == 0 || !found) {
                outBase = start - offset;
                outPath = path;
                found = true;
                if (offset == 0) break;
            }
        }
    }
    fclose(f);
    return found;
}

static bool ResolveElfDynSymbolOffset(const std::string& elfPath, const char* symName, uint64_t& outRelOffset) {
    int fd = open(elfPath.c_str(), O_RDONLY);
    if (fd < 0) return false;

    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(Elf64_Ehdr))) {
        close(fd);
        return false;
    }

    std::vector<uint8_t> buf(static_cast<size_t>(st.st_size));
    ssize_t total = 0;
    while (total < st.st_size) {
        ssize_t n = read(fd, buf.data() + total, static_cast<size_t>(st.st_size - total));
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    if (total != st.st_size) return false;

    const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(buf.data());
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64) {
        return false;
    }

    const auto* ph = reinterpret_cast<const Elf64_Phdr*>(buf.data() + eh->e_phoff);
    uint64_t minVaddr = UINT64_MAX;
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr < minVaddr) {
            minVaddr = ph[i].p_vaddr;
        }
    }
    if (minVaddr == UINT64_MAX) minVaddr = 0;
    const uint64_t biasBase = minVaddr & ~4095ULL;

    if (eh->e_shoff == 0 || eh->e_shnum == 0) return false;
    const auto* sh = reinterpret_cast<const Elf64_Shdr*>(buf.data() + eh->e_shoff);
    for (uint16_t i = 0; i < eh->e_shnum; ++i) {
        if (sh[i].sh_type != SHT_DYNSYM && sh[i].sh_type != SHT_SYMTAB) continue;
        if (sh[i].sh_link >= eh->e_shnum) continue;
        const auto& strSh = sh[sh[i].sh_link];
        if (sh[i].sh_offset + sh[i].sh_size > buf.size() || strSh.sh_offset + strSh.sh_size > buf.size()) continue;

        const auto* syms = reinterpret_cast<const Elf64_Sym*>(buf.data() + sh[i].sh_offset);
        const size_t count = sh[i].sh_size / sizeof(Elf64_Sym);
        const char* strs = reinterpret_cast<const char*>(buf.data() + strSh.sh_offset);
        for (size_t j = 0; j < count; ++j) {
            if (syms[j].st_name < strSh.sh_size && syms[j].st_value != 0 &&
                strcmp(strs + syms[j].st_name, symName) == 0) {
                outRelOffset = syms[j].st_value - biasBase;
                return true;
            }
        }
    }
    return false;
}

static bool PtraceRead(pid_t pid, uint64_t addr, void* dst, size_t len) {
    struct iovec local{dst, len};
    struct iovec remote{reinterpret_cast<void*>(addr), len};
    if (process_vm_readv(pid, &local, 1, &remote, 1, 0) == static_cast<ssize_t>(len)) {
        return true;
    }
    uint8_t* p = static_cast<uint8_t*>(dst);
    for (size_t off = 0; off < len; off += sizeof(long)) {
        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, pid, reinterpret_cast<void*>(addr + off), nullptr);
        if (errno != 0) return false;
        size_t chunk = (len - off < sizeof(long)) ? (len - off) : sizeof(long);
        memcpy(p + off, &word, chunk);
    }
    return true;
}

static bool PtraceWrite(pid_t pid, uint64_t addr, const void* src, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(src);
    for (size_t off = 0; off < len; off += sizeof(long)) {
        long word = 0;
        size_t chunk = (len - off < sizeof(long)) ? (len - off) : sizeof(long);
        if (chunk < sizeof(long)) {
            errno = 0;
            word = ptrace(PTRACE_PEEKDATA, pid, reinterpret_cast<void*>(addr + off), nullptr);
            if (errno != 0) return false;
        }
        memcpy(&word, p + off, chunk);
        if (ptrace(PTRACE_POKEDATA, pid, reinterpret_cast<void*>(addr + off), reinterpret_cast<void*>(word)) != 0) {
            return false;
        }
    }
    return true;
}

static bool GetRegs(pid_t pid, struct user_pt_regs& regs) {
    struct iovec iov{&regs, sizeof(regs)};
    return ptrace(PTRACE_GETREGSET, pid, reinterpret_cast<void*>(NT_PRSTATUS), &iov) == 0;
}

static bool SetRegs(pid_t pid, const struct user_pt_regs& regs) {
    struct iovec iov{const_cast<struct user_pt_regs*>(&regs), sizeof(regs)};
    return ptrace(PTRACE_SETREGSET, pid, reinterpret_cast<void*>(NT_PRSTATUS), &iov) == 0;
}

static bool RemoteCall4(pid_t pid, struct user_pt_regs& callRegs, uint64_t fnAddr,
                        uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t& outRet) {
    callRegs.regs[0] = a0;
    callRegs.regs[1] = a1;
    callRegs.regs[2] = a2;
    callRegs.regs[3] = a3;
    callRegs.regs[30] = 0; // return trap address -> SIGSEGV at pc==0
    callRegs.pc = fnAddr;

    if (!SetRegs(pid, callRegs)) return false;
    if (ptrace(PTRACE_CONT, pid, nullptr, nullptr) != 0) return false;

    int status = 0;
    while (waitpid(pid, &status, WUNTRACED) > 0) {
        if (WIFSTOPPED(status)) {
            int sig = WSTOPSIG(status);
            if (sig == SIGSEGV || sig == SIGTRAP) {
                struct user_pt_regs retRegs{};
                if (!GetRegs(pid, retRegs)) return false;
                if (retRegs.pc == 0 || sig == SIGSEGV) {
                    outRet = retRegs.regs[0];
                    return retRegs.pc == 0;
                }
            }
            // Forward non-call signals to target while waiting for our call to return
            if (ptrace(PTRACE_CONT, pid, nullptr, reinterpret_cast<void*>(static_cast<long>(sig))) != 0) {
                return false;
            }
        } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
            return false;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <pid> <so_path>\n", argv[0]);
        return 1;
    }

    pid_t pid = static_cast<pid_t>(atoi(argv[1]));
    const char* soPath = argv[2];
    if (pid <= 0 || !soPath[0]) {
        fprintf(stderr, "[!] Invalid arguments.\n");
        return 1;
    }

    uint64_t linkerBase = 0;
    std::string linkerPath;
    if (!FindRemoteModule(pid, "linker64", linkerBase, linkerPath)) {
        fprintf(stderr, "[!] Could not find linker64 in /proc/%d/maps\n", pid);
        return 1;
    }

    uint64_t loaderDlopenOff = 0, loaderDlerrorOff = 0;
    if (!ResolveElfDynSymbolOffset(linkerPath, "__loader_dlopen", loaderDlopenOff)) {
        fprintf(stderr, "[!] Could not resolve __loader_dlopen in %s\n", linkerPath.c_str());
        return 1;
    }
    ResolveElfDynSymbolOffset(linkerPath, "__loader_dlerror", loaderDlerrorOff);

    uint64_t callerAddr = 0;
    std::string callerPath;
    if (!FindRemoteModule(pid, "libunity.so", callerAddr, callerPath) &&
        !FindRemoteModule(pid, "libil2cpp.so", callerAddr, callerPath) &&
        !FindRemoteModule(pid, "libc.so", callerAddr, callerPath)) {
        callerAddr = linkerBase;
    }

    uint64_t remoteDlopen = linkerBase + loaderDlopenOff;
    uint64_t remoteDlerror = loaderDlerrorOff ? (linkerBase + loaderDlerrorOff) : 0;

    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) != 0) {
        perror("[!] PTRACE_ATTACH failed");
        return 1;
    }

    int status = 0;
    if (waitpid(pid, &status, WUNTRACED) < 0 || !WIFSTOPPED(status)) {
        perror("[!] waitpid failed after attach");
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return 1;
    }

    struct user_pt_regs origRegs{};
    if (!GetRegs(pid, origRegs)) {
        perror("[!] PTRACE_GETREGSET failed");
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return 1;
    }

    struct user_pt_regs workRegs = origRegs;
    uint64_t strAddr = (origRegs.sp - 512ULL) & ~15ULL;
    size_t pathLen = strlen(soPath) + 1;
    if (pathLen > 480 || !PtraceWrite(pid, strAddr, soPath, pathLen)) {
        fprintf(stderr, "[!] Failed writing .so path to target stack\n");
        SetRegs(pid, origRegs);
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return 1;
    }
    workRegs.sp = strAddr - 64ULL;
    workRegs.pstate &= ~(3ULL << 10); // clear PSTATE.BTYPE for BTI targets

    uint64_t handle = 0;
    bool ok = RemoteCall4(pid, workRegs, remoteDlopen, strAddr, RTLD_NOW, callerAddr, 0, handle);

    std::string errText;
    if (ok && handle == 0 && remoteDlerror != 0) {
        uint64_t errPtr = 0;
        workRegs = origRegs;
        workRegs.sp = strAddr - 64ULL;
        if (RemoteCall4(pid, workRegs, remoteDlerror, 0, 0, 0, 0, errPtr) && errPtr != 0) {
            char errBuf[256] = {};
            if (PtraceRead(pid, errPtr, errBuf, sizeof(errBuf) - 1)) {
                errText = errBuf;
            }
        }
    }

    SetRegs(pid, origRegs);
    ptrace(PTRACE_DETACH, pid, nullptr, nullptr);

    if (!ok || handle == 0) {
        fprintf(stderr, "[!] Remote dlopen failed (ok=%d, handle=0x%lx%s%s)\n",
                ok ? 1 : 0, static_cast<unsigned long>(handle),
                errText.empty() ? "" : ", dlerror=", errText.c_str());
        return 1;
    }

    printf("[+] Injected %s into PID %d (handle=0x%lx)\n",
           soPath, pid, static_cast<unsigned long>(handle));
    return 0;
}

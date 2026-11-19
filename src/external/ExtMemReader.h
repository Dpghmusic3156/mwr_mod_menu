#pragma once

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

class ExtMemReader {
public:
    static constexpr uint64_t kPtrTagMask = 0x00ffffffffffffffULL;

    struct MapsRegion {
        uint64_t    start = 0;
        uint64_t    end = 0;
        char        perms[8]{};
        std::string path;
    };

    ~ExtMemReader() { Close(); }

    bool Attach(pid_t pid) {
        if (pid <= 0) return false;
        if (m_pid == pid && m_fd >= 0) return true;
        Close();
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/mem", static_cast<int>(pid));
        m_fd = open(path, O_RDONLY);
        if (m_fd < 0) return false;
        m_pid = pid;
        return true;
    }

    void Close() {
        CloseWrite();
        if (m_fd >= 0) {
            close(m_fd);
            m_fd = -1;
        }
        m_pid = -1;
    }

    void CloseWrite() {
        if (m_wfd >= 0) {
            close(m_wfd);
            m_wfd = -1;
        }
    }

    bool IsAttached() const { return m_fd >= 0 && m_pid > 0; }
    bool IsWriteEnabled() const { return m_wfd >= 0 && m_pid > 0; }
    pid_t Pid() const { return m_pid; }

    static inline uint64_t Untag(uint64_t p) { return p & kPtrTagMask; }

    static inline bool IsValidAddrRange(uint64_t p) {
        const uint64_t u = p & kPtrTagMask;
        return u >= 0x10000000ULL && u < 0x0000ffffffffffffULL;
    }

    static inline bool IsValidUserPtr(uint64_t p) {
        const uint64_t u = p & kPtrTagMask;
        return u >= 0x10000000ULL && u < 0x0000ffffffffffffULL && (u & 3ULL) == 0;
    }

    bool ReadMem(uint64_t addr, void* dst, size_t len) const {
        if (m_fd < 0 || !IsValidAddrRange(addr) || len == 0 || len > (32ULL * 1024ULL * 1024ULL)) {
            return false;
        }
        const uint64_t raw = Untag(addr);
        ssize_t n = pread64(m_fd, dst, len, static_cast<off64_t>(raw));
        return n == static_cast<ssize_t>(len);
    }

    bool WriteMem(uint64_t addr, const void* src, size_t len) {
        if (m_pid <= 0 || !IsValidAddrRange(addr) || len == 0 || len > 512U) {
            return false;
        }
        if (m_wfd < 0) {
            char path[64];
            std::snprintf(path, sizeof(path), "/proc/%d/mem", static_cast<int>(m_pid));
            m_wfd = open(path, O_RDWR);
            if (m_wfd < 0) return false;
        }
        const uint64_t raw = Untag(addr);
        ssize_t n = pwrite64(m_wfd, src, len, static_cast<off64_t>(raw));
        return n == static_cast<ssize_t>(len);
    }

    template <typename T>
    inline T ReadVal(uint64_t addr, T fallback = T{}) const {
        T v{};
        return ReadMem(addr, &v, sizeof(T)) ? v : fallback;
    }

    inline uint64_t ReadPtr(uint64_t addr) const {
        uint64_t p = 0;
        if (!ReadMem(addr, &p, sizeof(p))) return 0;
        return IsValidUserPtr(p) ? p : 0;
    }

    bool ReadChunked(uint64_t addr, void* dst, size_t len) const {
        if (m_fd < 0 || !IsValidAddrRange(addr) || len == 0) return false;
        auto* out = static_cast<uint8_t*>(dst);
        size_t done = 0;
        while (done < len) {
            const size_t step = std::min<size_t>(4096U, len - done);
            const ssize_t n = pread64(m_fd, out + done, step, static_cast<off64_t>(Untag(addr) + done));
            if (n <= 0) {
                std::memset(out + done, 0, step);
            }
            done += step;
        }
        return true;
    }

    std::vector<MapsRegion> ReadMaps() const {
        std::vector<MapsRegion> out;
        if (m_pid <= 0) return out;
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/maps", static_cast<int>(m_pid));
        FILE* f = fopen(path, "r");
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

private:
    int   m_fd = -1;
    int   m_wfd = -1;
    pid_t m_pid = -1;
};

#include "ExtTouch.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "imgui.h"

namespace {

static constexpr int kMaxDevs = 5;
static constexpr int kMaxSlots = 10;

struct FingerSlot {
    bool isDown = false;
    int  rawX = 0;
    int  rawY = 0;
};

struct ReaderArg {
    int   idx = 0;
    float s2tx = 1.0f;
    float s2ty = 1.0f;
};

static int              g_fds[kMaxDevs]{};
static int              g_fdCount = 0;
static ReaderArg        g_args[kMaxDevs]{};
static FingerSlot       g_fingers[kMaxDevs][kMaxSlots]{};
static std::atomic<bool> g_running{false};

static std::mutex       g_touchMtx;
static float            g_curX = 0.0f;
static float            g_curY = 0.0f;
static bool             g_curDown = false;

static int              g_screenW = 1080;
static int              g_screenH = 2376;
static uint32_t         g_ori = 0;
static float            g_scaleX = 1.0f;
static float            g_scaleY = 1.0f;
static int              g_touchMaxX = 1;
static int              g_touchMaxY = 1;

static bool IsTouchDevice(int fd) {
    uint8_t* bits = nullptr;
    ssize_t bitsSize = 0;
    int res = 0;
    while (true) {
        res = ioctl(fd, EVIOCGBIT(EV_ABS, bitsSize), bits);
        if (res < bitsSize) break;
        bitsSize = res + 16;
        bits = static_cast<uint8_t*>(std::realloc(bits, static_cast<size_t>(bitsSize) * 2));
        if (!bits) return false;
    }
    bool hasSlot = false, hasX = false, hasY = false;
    struct input_absinfo abs{};
    for (int j = 0; j < res; ++j) {
        for (int k = 0; k < 8; ++k) {
            if ((bits[j] & (1 << k)) && ioctl(fd, EVIOCGABS(j * 8 + k), &abs) == 0) {
                const int code = j * 8 + k;
                if (code == ABS_MT_SLOT) hasSlot = true;
                else if (code == ABS_MT_POSITION_X) hasX = true;
                else if (code == ABS_MT_POSITION_Y) hasY = true;
            }
        }
    }
    std::free(bits);
    return hasSlot && hasX && hasY;
}

static void RecomputeScale() {
    const int shortEdge = std::max(1, std::min(g_screenW, g_screenH));
    const int longEdge  = std::max(1, std::max(g_screenW, g_screenH));
    g_scaleX = static_cast<float>(g_touchMaxX) / static_cast<float>(shortEdge);
    g_scaleY = static_cast<float>(g_touchMaxY) / static_cast<float>(longEdge);
}

static void* TouchReaderThread(void* arg) {
    const ReaderArg ra = *static_cast<const ReaderArg*>(arg);
    const int fd = g_fds[ra.idx];
    int slot = 0;
    struct input_event evs[64]{};

    while (g_running.load()) {
        const ssize_t n = read(fd, evs, sizeof(evs));
        if (n <= 0 || (n % static_cast<ssize_t>(sizeof(struct input_event))) != 0) {
            usleep(1000);
            continue;
        }
        const size_t count = static_cast<size_t>(n) / sizeof(struct input_event);
        for (size_t j = 0; j < count; ++j) {
            const struct input_event& ie = evs[j];
            if (ie.type == EV_ABS) {
                if (ie.code == ABS_MT_SLOT) {
                    if (ie.value >= 0 && ie.value < kMaxSlots) slot = ie.value;
                } else if (ie.code == ABS_MT_TRACKING_ID) {
                    g_fingers[ra.idx][slot].isDown = (ie.value != -1);
                } else if (ie.code == ABS_MT_POSITION_X) {
                    g_fingers[ra.idx][slot].rawX = static_cast<int>(ie.value * ra.s2tx);
                } else if (ie.code == ABS_MT_POSITION_Y) {
                    g_fingers[ra.idx][slot].rawY = static_cast<int>(ie.value * ra.s2ty);
                }
            } else if (ie.type == EV_SYN && ie.code == SYN_REPORT) {
                const FingerSlot& f = g_fingers[ra.idx][slot];
                std::lock_guard<std::mutex> lk(g_touchMtx);
                if (f.isDown) {
                    const float xt = static_cast<float>(f.rawX) / g_scaleX;
                    const float yt = static_cast<float>(f.rawY) / g_scaleY;
                    float sx = xt, sy = yt;
                    switch (g_ori) {
                        case 1:
                            sx = yt;
                            sy = static_cast<float>(g_screenH) - xt;
                            break;
                        case 2:
                            sx = static_cast<float>(g_screenW) - xt;
                            sy = static_cast<float>(g_screenH) - yt;
                            break;
                        case 3:
                            sx = static_cast<float>(g_screenW) - yt;
                            sy = xt;
                            break;
                        default:
                            sx = xt;
                            sy = yt;
                            break;
                    }
                    g_curX = sx;
                    g_curY = sy;
                    g_curDown = true;
                } else {
                    bool anyDown = false;
                    for (int s = 0; s < kMaxSlots; ++s) {
                        if (g_fingers[ra.idx][s].isDown) { anyDown = true; break; }
                    }
                    if (!anyDown) g_curDown = false;
                }
            }
        }
    }
    return nullptr;
}

} // namespace

bool ExtTouch_Init(int width, int height, uint32_t orientation) {
    DIR* dir = opendir("/dev/input");
    if (!dir) return false;
    int maxEventIdx = 0;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (std::strncmp(ent->d_name, "event", 5) == 0) {
            const int idx = std::atoi(ent->d_name + 5);
            if (idx > maxEventIdx) maxEventIdx = idx;
        }
    }
    closedir(dir);

    struct input_absinfo absX[kMaxDevs]{}, absY[kMaxDevs]{};
    g_fdCount = 0;
    int minIdx = maxEventIdx + 1;

    for (int i = 0; i <= maxEventIdx && g_fdCount < kMaxDevs; ++i) {
        char path[64];
        std::snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (IsTouchDevice(fd) &&
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &absX[g_fdCount]) == 0 &&
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &absY[g_fdCount]) == 0) {
            // Switch back to blocking reads cleanly for dedicated reader thread
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
            g_fds[g_fdCount] = fd;
            if (i < minIdx) {
                g_touchMaxX = std::max(1, absX[g_fdCount].maximum);
                g_touchMaxY = std::max(1, absY[g_fdCount].maximum);
                minIdx = i;
            }
            ++g_fdCount;
        } else {
            close(fd);
        }
    }

    if (g_fdCount == 0) return false;

    g_screenW = width;
    g_screenH = height;
    g_ori = orientation;
    RecomputeScale();

    g_running.store(true);
    for (int i = 0; i < g_fdCount; ++i) {
        g_args[i].idx = i;
        g_args[i].s2tx = static_cast<float>(g_touchMaxX) / static_cast<float>(std::max(1, absX[i].maximum));
        g_args[i].s2ty = static_cast<float>(g_touchMaxY) / static_cast<float>(std::max(1, absY[i].maximum));
        pthread_t th{};
        if (pthread_create(&th, nullptr, TouchReaderThread, &g_args[i]) == 0) {
            pthread_detach(th);
        }
    }
    return true;
}

void ExtTouch_UpdateScreen(int width, int height, uint32_t orientation) {
    std::lock_guard<std::mutex> lk(g_touchMtx);
    g_screenW = width;
    g_screenH = height;
    g_ori = orientation;
    RecomputeScale();
}

void ExtTouch_FeedImGui() {
    ImGuiIO& io = ImGui::GetIO();
    std::lock_guard<std::mutex> lk(g_touchMtx);
    io.AddMousePosEvent(g_curX, g_curY);
    io.AddMouseButtonEvent(0, g_curDown);
}

void ExtTouch_Close() {
    g_running.store(false);
    for (int i = 0; i < g_fdCount; ++i) {
        if (g_fds[i] > 0) {
            close(g_fds[i]);
            g_fds[i] = 0;
        }
    }
    g_fdCount = 0;
    std::memset(g_fingers, 0, sizeof(g_fingers));
}

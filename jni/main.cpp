#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <linux/input.h>
#include <poll.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <thread>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

#define LOG_TAG "SimpleImGui"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int kScanTargets = 64;
constexpr int kMaxEspEntities = 64;
constexpr const char* kEspProcessToken = "com.mobile.legends";
constexpr const char* kEspModuleName = "libil2cpp.so";

std::atomic<bool> gRunning{true};
std::atomic<bool> gTouchReady{false};
std::atomic<bool> gTouchSeen{false};
std::atomic<bool> gTouchDown{false};
std::atomic<int> gTouchX{0};
std::atomic<int> gTouchY{0};
volatile sig_atomic_t gSignalStop = 0;

struct MenuConfig {
    bool menuExpanded = true;
    bool showCrosshair = false;
    float crosshairSize = 25.0f;
    float crosshairThickness = 3.0f;
    bool enableEsp = false;
    bool espBox = true;
    bool espLine = false;
    bool espHealth = true;
    bool espDistance = true;
    float espMaxDistance = 300.0f;
    float espColor[4] = {1.0f, 0.2f, 0.2f, 1.0f};
    ImU64 espStaticRva = 0;
    int counter = 0;
    int selectedMode = 0;
};

struct Offsets {
    // BattleManager.Instance; // 0x0
    uintptr_t battleManagerInstance = 0x0;
    // BattleManager.m_ShowPlayers; // 0x70
    uintptr_t entityList = 0x70;
    // BattleManager.m_LocalPlayerShow; // 0x48
    uintptr_t localPlayer = 0x48;
    // ShowEntity.IsPlayer; // 0x8B
    uintptr_t entityIsPlayer = 0x8B;
    // ShowEntity.m_bDeath; // 0xC5
    uintptr_t entityDead = 0xC5;
    // ShowEntity.m_EntityCampType; // 0xD0
    uintptr_t entityTeam = 0xD0;
    // ShowEntity.m_Hp; // 0x1A4
    uintptr_t entityHealth = 0x1A4;
    // ShowEntity.m_HpMax; // 0x1A8
    uintptr_t entityMaxHealth = 0x1A8;
    // ShowEntity.m_vUnityCachePosition; // 0x298
    uintptr_t entityPosition = 0x298;
    // ShowPlayer.m_ConfigSize; // 0x8E0
    uintptr_t playerConfigSize = 0x8E0;
    // ShowPlayer.m_HeroName; // 0x8F0
    uintptr_t playerName = 0x8F0;
    // ShowPlayer.m_iTeamId; // 0x908
    uintptr_t playerTeamId = 0x908;
    // ShowBattleCamera.m_CamPos; // 0x10
    uintptr_t cameraPosition = 0x10;
    // ShowBattleCamera.m_CamRotVect; // 0x28
    uintptr_t cameraRotationVector = 0x28;
    // ShowBattleCamera.m_CamRot; // 0x58
    uintptr_t cameraRotation = 0x58;
    // ShowBattleCamera.m_iFieldOfView; // 0x8C
    uintptr_t cameraFov = 0x8C;
    // MTCameraData.viewMatrix; // 0xD0
    uintptr_t cameraViewMatrix = 0xD0;
    // MTCameraData.projectionMatrix; // 0x110
    uintptr_t cameraProjectionMatrix = 0x110;
    // MTCameraData.pixelWidth; // 0x190
    uintptr_t cameraPixelWidth = 0x190;
    // MTCameraData.pixelHeight; // 0x194
    uintptr_t cameraPixelHeight = 0x194;
    // System.Collections.Generic.List<T>._items, IL2CPP layout, not a dump.cs field
    uintptr_t listItems = 0x10;
    // System.Collections.Generic.List<T>._size, IL2CPP layout, not a dump.cs field
    uintptr_t listCount = 0x18;
    // TODO: tidak ditemukan di dump.cs, alamat tetap blok static fields tidak tersedia
    uintptr_t staticFieldsAddress = 0x0;
};

MenuConfig gConfig;
Offsets gOffsets;
std::string gEspStatus = "ESP: menunggu target";

std::mutex gImGuiMutex;
ImGuiIO* gImGuiIo = nullptr;

struct TouchDevice {
    int fd = -1;
    std::string path;
    std::string name;
    int minX = 0;
    int maxX = 0;
    int minY = 0;
    int maxY = 0;
    bool hasTrackingId = false;
};

struct EglState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
};

bool shouldRun() {
    return gRunning.load() && gSignalStop == 0;
}

void handleSignal(int) {
    gSignalStop = 1;
}

bool hasBit(const uint8_t* bits, int bit) {
    return (bits[bit / 8] & (1u << (bit % 8))) != 0;
}

int mapRange(int value, int minimum, int maximum, int size) {
    if (maximum <= minimum || size <= 1) {
        return 0;
    }
    float normalized = static_cast<float>(value - minimum) /
                       static_cast<float>(maximum - minimum);
    normalized = std::clamp(normalized, 0.0f, 1.0f);
    return static_cast<int>(normalized * static_cast<float>(size - 1));
}

ImVec2 mapTouch(const TouchDevice& device, int rawX, int rawY, int width, int height) {
    const bool screenLandscape = width > height;
    const bool rawLandscape = (device.maxX - device.minX) > (device.maxY - device.minY);
    const bool swapAxes = screenLandscape != rawLandscape;

    if (swapAxes) {
        return ImVec2(static_cast<float>(mapRange(rawY, device.minY, device.maxY, width)),
                      static_cast<float>(mapRange(rawX, device.minX, device.maxX, height)));
    }
    return ImVec2(static_cast<float>(mapRange(rawX, device.minX, device.maxX, width)),
                  static_cast<float>(mapRange(rawY, device.minY, device.maxY, height)));
}

TouchDevice findTouchDevice() {
    TouchDevice best;
    int bestScore = -1;

    for (int index = 0; index < kScanTargets; ++index) {
        char path[64];
        std::snprintf(path, sizeof(path), "/dev/input/event%d", index);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        uint8_t eventBits[(EV_MAX + 7) / 8] = {};
        uint8_t absBits[(ABS_MAX + 7) / 8] = {};
        uint8_t keyBits[(KEY_MAX + 7) / 8] = {};
        uint8_t propBits[(INPUT_PROP_MAX + 7) / 8] = {};

        bool usable = ioctl(fd, EVIOCGBIT(0, sizeof(eventBits)), eventBits) >= 0 &&
                      hasBit(eventBits, EV_ABS);
        if (usable) {
            usable = ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits) >= 0 &&
                     hasBit(absBits, ABS_MT_POSITION_X) &&
                     hasBit(absBits, ABS_MT_POSITION_Y);
        }
        if (!usable) {
            close(fd);
            continue;
        }

        input_absinfo xInfo{};
        input_absinfo yInfo{};
        if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &xInfo) < 0 ||
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &yInfo) < 0 ||
            xInfo.maximum <= xInfo.minimum || yInfo.maximum <= yInfo.minimum) {
            close(fd);
            continue;
        }

        char nameBuffer[256] = {};
        ioctl(fd, EVIOCGNAME(sizeof(nameBuffer) - 1), nameBuffer);

        bool hasButton = false;
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) >= 0) {
            hasButton = hasBit(keyBits, BTN_TOUCH);
        }

        bool isDirect = false;
        if (ioctl(fd, EVIOCGPROP(sizeof(propBits)), propBits) >= 0) {
            isDirect = hasBit(propBits, INPUT_PROP_DIRECT);
        }

        int score = 1;
        score += isDirect ? 100 : 0;
        score += hasButton ? 20 : 0;
        score += (xInfo.maximum - xInfo.minimum) / 1000;
        score += (yInfo.maximum - yInfo.minimum) / 1000;

        if (score > bestScore) {
            if (best.fd >= 0) {
                close(best.fd);
            }
            best.fd = fd;
            best.path = path;
            best.name = nameBuffer;
            best.minX = xInfo.minimum;
            best.maxX = xInfo.maximum;
            best.minY = yInfo.minimum;
            best.maxY = yInfo.maximum;
            best.hasTrackingId = hasBit(absBits, ABS_MT_TRACKING_ID);
            bestScore = score;
        } else {
            close(fd);
        }
    }

    return best;
}

void publishTouch(const TouchDevice& device, int rawX, int rawY, bool down,
                  int width, int height) {
    const ImVec2 position = mapTouch(device, rawX, rawY, width, height);
    const int screenX = static_cast<int>(position.x);
    const int screenY = static_cast<int>(position.y);

    gTouchX.store(screenX);
    gTouchY.store(screenY);
    gTouchDown.store(down);
    gTouchSeen.store(true);

    std::lock_guard<std::mutex> lock(gImGuiMutex);
    if (gImGuiIo != nullptr) {
        gImGuiIo->AddMousePosEvent(position.x, position.y);
        gImGuiIo->AddMouseButtonEvent(0, down);
    }
}

void touchLoop(TouchDevice device, int width, int height) {
    gTouchReady.store(true);
    LOGI("Touch device %s (%s) raw %d..%d x %d..%d, tracking=%d",
         device.path.c_str(), device.name.c_str(),
         device.minX, device.maxX, device.minY, device.maxY,
         device.hasTrackingId ? 1 : 0);

    int currentSlot = 0;
    int rawX = (device.minX + device.maxX) / 2;
    int rawY = (device.minY + device.maxY) / 2;
    bool trackingDown = false;
    bool buttonDown = false;

    while (shouldRun()) {
        pollfd descriptor{device.fd, POLLIN, 0};
        int pollResult = poll(&descriptor, 1, 250);
        if (pollResult < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOGE("poll failed on %s: %s", device.path.c_str(), std::strerror(errno));
            break;
        }
        if (pollResult == 0) {
            continue;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            LOGW("Touch device %s disconnected", device.path.c_str());
            break;
        }

        input_event event{};
        ssize_t received = read(device.fd, &event, sizeof(event));
        if (received != static_cast<ssize_t>(sizeof(event))) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            LOGE("read failed on %s: %s", device.path.c_str(), std::strerror(errno));
            break;
        }

        if (event.type == EV_ABS) {
            if (event.code == ABS_MT_SLOT) {
                currentSlot = event.value;
            } else if (currentSlot == 0) {
                if (event.code == ABS_MT_POSITION_X) {
                    rawX = event.value;
                } else if (event.code == ABS_MT_POSITION_Y) {
                    rawY = event.value;
                } else if (event.code == ABS_MT_TRACKING_ID) {
                    trackingDown = event.value >= 0;
                }
            }
        } else if (event.type == EV_KEY && event.code == BTN_TOUCH) {
            buttonDown = event.value != 0;
            if (!buttonDown) {
                trackingDown = false;
            }
        } else if (event.type == EV_SYN && event.code == SYN_DROPPED) {
            trackingDown = false;
            buttonDown = false;
        } else if (event.type == EV_SYN && event.code == SYN_REPORT) {
            const bool down = device.hasTrackingId ? trackingDown : buttonDown;
            publishTouch(device, rawX, rawY, down, width, height);
        }
    }

    if (gTouchDown.load()) {
        std::lock_guard<std::mutex> lock(gImGuiMutex);
        if (gImGuiIo != nullptr) {
            gImGuiIo->AddMouseButtonEvent(0, false);
        }
    }
    gTouchDown.store(false);
    gTouchSeen.store(false);
    gTouchReady.store(false);
    close(device.fd);
}

bool initEgl(ANativeWindow* window, int width, int height, EglState* state) {
    state->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (state->display == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }
    if (eglInitialize(state->display, nullptr, nullptr) != EGL_TRUE) {
        LOGE("eglInitialize failed: 0x%x", eglGetError());
        return false;
    }
    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        LOGE("eglBindAPI failed: 0x%x", eglGetError());
        return false;
    }

    const EGLint configAttributes[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 0,
            EGL_STENCIL_SIZE, 0,
            EGL_NONE
    };
    EGLConfig config = nullptr;
    EGLint configCount = 0;
    if (eglChooseConfig(state->display, configAttributes, &config, 1, &configCount) != EGL_TRUE ||
        configCount < 1) {
        LOGE("eglChooseConfig failed: 0x%x", eglGetError());
        return false;
    }

    const EGLint surfaceAttributes[] = {EGL_NONE};
    state->surface = eglCreateWindowSurface(state->display, config, window, surfaceAttributes);
    if (state->surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
        return false;
    }

    const EGLint contextAttributes[] = {
            EGL_CONTEXT_CLIENT_VERSION, 3,
            EGL_NONE
    };
    state->context = eglCreateContext(state->display, config, EGL_NO_CONTEXT, contextAttributes);
    if (state->context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: 0x%x", eglGetError());
        return false;
    }
    if (eglMakeCurrent(state->display, state->surface, state->surface, state->context) != EGL_TRUE) {
        LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
        return false;
    }

    eglSwapInterval(state->display, 1);
    const char* eglVersion = eglQueryString(state->display, EGL_VERSION);
    const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    LOGI("EGL %s | GL %s", eglVersion != nullptr ? eglVersion : "unknown",
         glVersion != nullptr ? glVersion : "unknown");

    glViewport(0, 0, width, height);
    return true;
}

void shutdownEgl(EglState* state) {
    if (state->display == EGL_NO_DISPLAY) {
        return;
    }
    eglMakeCurrent(state->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (state->context != EGL_NO_CONTEXT) {
        eglDestroyContext(state->display, state->context);
    }
    if (state->surface != EGL_NO_SURFACE) {
        eglDestroySurface(state->display, state->surface);
    }
    eglTerminate(state->display);
    state->display = EGL_NO_DISPLAY;
    state->context = EGL_NO_CONTEXT;
    state->surface = EGL_NO_SURFACE;
}

void configureStyle(ImGuiIO& io) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(2.5f);
    style.WindowPadding = ImVec2(18.0f, 18.0f);
    style.ItemSpacing = ImVec2(12.0f, 10.0f);
    style.FramePadding = ImVec2(12.0f, 10.0f);
    style.WindowRounding = 6.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.06f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.14f, 0.14f, 1.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.13f, 0.13f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.22f, 0.22f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.86f, 0.86f, 0.82f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.78f, 0.78f, 0.74f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.95f, 0.95f, 0.90f, 1.0f);
    colors[ImGuiCol_Separator] = ImVec4(0.24f, 0.24f, 0.24f, 1.0f);

    io.FontGlobalScale = 2.2f;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.MouseDrawCursor = false;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
}

bool validRemotePointer(uintptr_t value) {
    return value >= 0x1000 && value <= 0x00007fffffffffffULL;
}

bool readRemote(pid_t pid, uintptr_t address, void* destination, size_t length) {
    if (pid <= 0 || address == 0 || destination == nullptr || length == 0) {
        return false;
    }

    iovec local{destination, length};
    iovec remote{reinterpret_cast<void*>(address), length};
    const ssize_t bytes = process_vm_readv(pid, &local, 1, &remote, 1, 0);
    return bytes == static_cast<ssize_t>(length);
}

pid_t findGameProcess() {
    DIR* directory = opendir("/proc");
    if (directory == nullptr) {
        return -1;
    }

    pid_t result = -1;
    dirent* entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        const char* name = entry->d_name;
        if (name[0] < '0' || name[0] > '9') {
            continue;
        }

        char path[64];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", name);
        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        char buffer[512];
        const ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);
        close(fd);
        if (bytes <= 0) {
            continue;
        }
        buffer[bytes] = '\0';

        if (std::strstr(buffer, kEspProcessToken) != nullptr) {
            result = static_cast<pid_t>(std::strtol(name, nullptr, 10));
            break;
        }
    }

    closedir(directory);
    return result;
}

uintptr_t findModuleBase(pid_t pid, const char* moduleName) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", static_cast<int>(pid));
    FILE* file = fopen(path, "r");
    if (file == nullptr) {
        return 0;
    }

    uintptr_t base = 0;
    char line[512];
    while (fgets(line, sizeof(line), file) != nullptr) {
        if (std::strstr(line, moduleName) == nullptr) {
            continue;
        }
        char* separator = std::strchr(line, '-');
        if (separator == nullptr) {
            continue;
        }
        *separator = '\0';
        base = static_cast<uintptr_t>(std::strtoull(line, nullptr, 16));
        break;
    }

    fclose(file);
    return validRemotePointer(base) ? base : 0;
}

void multiplyMatrix(const float left[16], const float right[16], float output[16]) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int index = 0; index < 4; ++index) {
                sum += left[index * 4 + row] * right[column * 4 + index];
            }
            output[column * 4 + row] = sum;
        }
    }
}

void buildViewFromQuaternion(const float position[3], const float quaternion[4],
                             float output[16]) {
    const float x = quaternion[0];
    const float y = quaternion[1];
    const float z = quaternion[2];
    const float w = quaternion[3];
    const float x2 = x + x;
    const float y2 = y + y;
    const float z2 = z + z;
    const float xx = x * x2;
    const float xy = x * y2;
    const float xz = x * z2;
    const float yy = y * y2;
    const float yz = y * z2;
    const float zz = z * z2;
    const float wx = w * x2;
    const float wy = w * y2;
    const float wz = w * z2;

    float rotation[16] = {
            1.0f - (yy + zz), xy + wz, xz - wy, 0.0f,
            xy - wz, 1.0f - (xx + zz), yz + wx, 0.0f,
            xz + wy, yz - wx, 1.0f - (xx + yy), 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f};

    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            output[column * 4 + row] = rotation[row * 4 + column];
        }
    }
    output[12] = -(rotation[0] * position[0] + rotation[1] * position[1] +
                    rotation[2] * position[2]);
    output[13] = -(rotation[4] * position[0] + rotation[5] * position[1] +
                    rotation[6] * position[2]);
    output[14] = -(rotation[8] * position[0] + rotation[9] * position[1] +
                    rotation[10] * position[2]);
    output[15] = 1.0f;
}

void buildPerspective(float fovDegrees, float aspect, float nearPlane, float farPlane,
                      float output[16]) {
    const float tangent = tanf(fovDegrees * 0.5f * 3.14159265f / 180.0f);
    const float focal = 1.0f / tangent;
    for (int index = 0; index < 16; ++index) {
        output[index] = 0.0f;
    }
    output[0] = focal / aspect;
    output[5] = focal;
    output[10] = (farPlane + nearPlane) / (nearPlane - farPlane);
    output[11] = -1.0f;
    output[14] = (2.0f * farPlane * nearPlane) / (nearPlane - farPlane);
}

bool worldToScreen(const float world[3], const float viewProjection[16], int width,
                   int height, ImVec2& screen, float& clipW) {
    const float x = world[0];
    const float y = world[1];
    const float z = world[2];
    const float clipX = viewProjection[0] * x + viewProjection[4] * y +
                        viewProjection[8] * z + viewProjection[12];
    const float clipY = viewProjection[1] * x + viewProjection[5] * y +
                        viewProjection[9] * z + viewProjection[13];
    clipW = viewProjection[3] * x + viewProjection[7] * y +
            viewProjection[11] * z + viewProjection[15];

    if (!std::isfinite(clipW) || clipW <= 0.001f) {
        return false;
    }

    const float ndcX = clipX / clipW;
    const float ndcY = clipY / clipW;
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) {
        return false;
    }

    screen.x = (ndcX * 0.5f + 0.5f) * static_cast<float>(width);
    screen.y = (0.5f - ndcY * 0.5f) * static_cast<float>(height);
    return std::isfinite(screen.x) && std::isfinite(screen.y);
}

void drawEsp(int width, int height) {
    static pid_t cachedPid = -1;
    static uintptr_t cachedModuleBase = 0;
    static auto lastProbe = std::chrono::steady_clock::time_point::min();
    const auto now = std::chrono::steady_clock::now();

    if (cachedPid <= 0 || cachedModuleBase == 0 ||
        now - lastProbe > std::chrono::seconds(2)) {
        cachedPid = findGameProcess();
        cachedModuleBase = cachedPid > 0 ? findModuleBase(cachedPid, kEspModuleName) : 0;
        lastProbe = now;
    }

    if (cachedPid <= 0) {
        gEspStatus = "ESP: target tidak ditemukan";
        return;
    }
    if (cachedModuleBase == 0) {
        gEspStatus = "ESP: libil2cpp.so tidak ditemukan";
        return;
    }

    uintptr_t staticBase = gOffsets.staticFieldsAddress;
    if (staticBase == 0) {
        if (gConfig.espStaticRva == 0) {
            gEspStatus = "ESP: RVA static fields belum diisi";
            return;
        }
        staticBase = cachedModuleBase + static_cast<uintptr_t>(gConfig.espStaticRva);
    }

    float cameraPosition[3] = {0.0f, 0.0f, 0.0f};
    float cameraQuaternion[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    int fovValue = 0;
    if (!readRemote(cachedPid, staticBase + gOffsets.cameraPosition, cameraPosition,
                    sizeof(cameraPosition)) ||
        !readRemote(cachedPid, staticBase + gOffsets.cameraRotation, cameraQuaternion,
                    sizeof(cameraQuaternion)) ||
        !readRemote(cachedPid, staticBase + gOffsets.cameraFov, &fovValue,
                    sizeof(fovValue))) {
        gEspStatus = "ESP: data kamera tidak terbaca";
        return;
    }

    for (float value : cameraPosition) {
        if (!std::isfinite(value)) {
            gEspStatus = "ESP: posisi kamera tidak valid";
            return;
        }
    }
    float norm = 0.0f;
    for (float value : cameraQuaternion) {
        if (!std::isfinite(value)) {
            gEspStatus = "ESP: rotasi kamera tidak valid";
            return;
        }
        norm += value * value;
    }
    norm = sqrtf(norm);
    if (!std::isfinite(norm) || norm < 0.0001f) {
        gEspStatus = "ESP: rotasi kamera tidak valid";
        return;
    }
    for (float& value : cameraQuaternion) {
        value /= norm;
    }

    float fov = static_cast<float>(fovValue);
    if (!std::isfinite(fov) || fov < 30.0f || fov > 120.0f) {
        fov = 60.0f;
    }
    if (height <= 0) {
        return;
    }

    float view[16] = {};
    float projection[16] = {};
    float viewProjection[16] = {};
    buildViewFromQuaternion(cameraPosition, cameraQuaternion, view);
    buildPerspective(fov, static_cast<float>(width) / static_cast<float>(height),
                     0.1f, 1000.0f, projection);
    multiplyMatrix(projection, view, viewProjection);

    uintptr_t manager = 0;
    if (!readRemote(cachedPid, staticBase + gOffsets.battleManagerInstance, &manager,
                    sizeof(manager)) ||
        !validRemotePointer(manager)) {
        gEspStatus = "ESP: BattleManager tidak tersedia";
        return;
    }

    uintptr_t localPlayer = 0;
    if (!readRemote(cachedPid, manager + gOffsets.localPlayer, &localPlayer,
                    sizeof(localPlayer))) {
        localPlayer = 0;
    }

    uintptr_t list = 0;
    uintptr_t items = 0;
    int count = 0;
    if (!readRemote(cachedPid, manager + gOffsets.entityList, &list, sizeof(list)) ||
        !validRemotePointer(list) ||
        !readRemote(cachedPid, list + gOffsets.listItems, &items, sizeof(items)) ||
        !readRemote(cachedPid, list + gOffsets.listCount, &count, sizeof(count)) ||
        !validRemotePointer(items) || count <= 0) {
        gEspStatus = "ESP: daftar entitas tidak terbaca";
        return;
    }
    count = std::min(count, kMaxEspEntities);

    const auto toByte = [](float value) {
        return static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
    };
    const ImU32 espColor = IM_COL32(toByte(gConfig.espColor[0]),
                                    toByte(gConfig.espColor[1]),
                                    toByte(gConfig.espColor[2]),
                                    toByte(gConfig.espColor[3]));
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    int drawn = 0;

    for (int index = 0; index < count; ++index) {
        uintptr_t entity = 0;
        if (!readRemote(cachedPid, items + sizeof(uintptr_t) * index, &entity,
                        sizeof(entity)) ||
            !validRemotePointer(entity) || entity == localPlayer) {
            continue;
        }

        uint8_t isPlayer = 0;
        uint8_t dead = 0;
        float position[3] = {0.0f, 0.0f, 0.0f};
        if (!readRemote(cachedPid, entity + gOffsets.entityIsPlayer, &isPlayer,
                        sizeof(isPlayer)) ||
            !readRemote(cachedPid, entity + gOffsets.entityDead, &dead, sizeof(dead)) ||
            isPlayer == 0 || dead != 0 ||
            !readRemote(cachedPid, entity + gOffsets.entityPosition, position,
                        sizeof(position))) {
            continue;
        }

        bool positionValid = true;
        for (float value : position) {
            positionValid = positionValid && std::isfinite(value);
        }
        if (!positionValid) {
            continue;
        }

        const float dx = position[0] - cameraPosition[0];
        const float dy = position[1] - cameraPosition[1];
        const float dz = position[2] - cameraPosition[2];
        const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(distance) || distance > gConfig.espMaxDistance) {
            continue;
        }

        int health = 0;
        int maxHealth = 0;
        float size = 0.0f;
        readRemote(cachedPid, entity + gOffsets.entityHealth, &health, sizeof(health));
        readRemote(cachedPid, entity + gOffsets.entityMaxHealth, &maxHealth,
                   sizeof(maxHealth));
        readRemote(cachedPid, entity + gOffsets.playerConfigSize, &size, sizeof(size));
        if (!std::isfinite(size) || size <= 0.0f || size > 20.0f) {
            size = 2.0f;
        }

        const float feetWorld[3] = {position[0], position[1], position[2]};
        const float headWorld[3] = {position[0], position[1] + size, position[2]};
        ImVec2 feet;
        ImVec2 head;
        float feetW = 0.0f;
        float headW = 0.0f;
        if (!worldToScreen(feetWorld, viewProjection, width, height, feet, feetW) ||
            !worldToScreen(headWorld, viewProjection, width, height, head, headW) ||
            feetW <= 0.001f || headW <= 0.001f) {
            continue;
        }
        if (feet.x < -static_cast<float>(width) || feet.x > 2.0f * width ||
            feet.y < -static_cast<float>(height) || feet.y > 2.0f * height) {
            continue;
        }

        const ImVec2 boxMin(std::min(head.x, feet.x) - 4.0f, head.y - 4.0f);
        const ImVec2 boxMax(std::max(head.x, feet.x) + 4.0f, feet.y + 4.0f);

        if (gConfig.espBox) {
            drawList->AddRect(boxMin, boxMax, espColor, 0.0f, 0, 1.0f);
        }
        if (gConfig.espLine) {
            drawList->AddLine(ImVec2(width * 0.5f, static_cast<float>(height)), feet,
                              espColor, 1.0f);
        }
        if (gConfig.espHealth && maxHealth > 0) {
            const float ratio = std::clamp(static_cast<float>(health) /
                                           static_cast<float>(maxHealth), 0.0f, 1.0f);
            const ImVec2 barMin(boxMin.x, boxMin.y - 10.0f);
            const ImVec2 barMax(boxMax.x, boxMin.y - 4.0f);
            drawList->AddRectFilled(barMin, barMax, IM_COL32(18, 18, 18, 210));
            drawList->AddRectFilled(barMin,
                                    ImVec2(barMin.x + (barMax.x - barMin.x) * ratio,
                                           barMax.y),
                                    espColor);
        }
        if (gConfig.espDistance) {
            char label[32];
            snprintf(label, sizeof(label), "%.0f u", distance);
            drawList->AddText(ImVec2(boxMax.x + 4.0f, feet.y - 14.0f), espColor, label);
        }
        ++drawn;
    }

    gEspStatus = "ESP: " + std::to_string(drawn) + " entitas terlihat";
}

void drawFloatingToggle() {
    ImGui::SetNextWindowPos(ImVec2(36.0f, 120.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Menu Toggle", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings)) {
        const char* label = gConfig.menuExpanded ? "Hide Menu" : "Show Menu";
        if (ImGui::Button(label)) {
            gConfig.menuExpanded = !gConfig.menuExpanded;
        }
        ImGui::End();
    } else {
        ImGui::End();
    }
}

void drawVisualTab() {
    ImGui::Checkbox("Tampilkan Crosshair", &gConfig.showCrosshair);
    if (!gConfig.showCrosshair) {
        return;
    }

    const float controlWidth = ImGui::GetContentRegionAvail().x * 0.50f;
    ImGui::SetNextItemWidth(controlWidth);
    ImGui::SliderFloat("Ukuran", &gConfig.crosshairSize, 10.0f, 80.0f, "%.0f");
    ImGui::SetNextItemWidth(controlWidth);
    ImGui::SliderFloat("Tebal Garis", &gConfig.crosshairThickness, 1.0f, 10.0f, "%.1f");
}

void drawEspTab() {
    ImGui::Checkbox("Aktifkan External ESP", &gConfig.enableEsp);
    if (!gConfig.enableEsp) {
        return;
    }

    ImGui::Checkbox("Box ESP", &gConfig.espBox);
    ImGui::Checkbox("Snapline", &gConfig.espLine);
    ImGui::Checkbox("Health Bar", &gConfig.espHealth);
    ImGui::Checkbox("Jarak (Distance)", &gConfig.espDistance);

    const float controlWidth = ImGui::GetContentRegionAvail().x * 0.50f;
    ImGui::SetNextItemWidth(controlWidth);
    ImGui::SliderFloat("Jarak Maksimal", &gConfig.espMaxDistance, 50.0f, 500.0f, "%.0f");
    ImGui::SetNextItemWidth(controlWidth);
    ImGui::InputScalar("RVA Static", ImGuiDataType_U64, &gConfig.espStaticRva,
                       nullptr, nullptr, "0x%llX");
    ImGui::ColorEdit4("Warna ESP", gConfig.espColor);
}

void drawControlTab() {
    const char* modes[] = {"Mode Normal", "Mode Cepat", "Mode Hemat"};
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.50f);
    ImGui::Combo("Mode", &gConfig.selectedMode, modes, IM_ARRAYSIZE(modes));
    ImGui::Text("Counter: %d", gConfig.counter);
    if (ImGui::Button("Tambah +1")) {
        ++gConfig.counter;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        gConfig.counter = 0;
    }
}

bool drawSystemTab(int width, int height) {
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::Text("Resolution: %d x %d", width, height);

    if (!gTouchReady.load()) {
        ImGui::Text("Touch: unavailable");
    } else if (!gTouchSeen.load()) {
        ImGui::Text("Touch: waiting for input");
    } else {
        ImGui::Text("Touch: (%d, %d)%s", gTouchX.load(), gTouchY.load(),
                    gTouchDown.load() ? " down" : "");
    }

    ImGui::Text("ESP: %s", gEspStatus.c_str());

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.63f, 0.16f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.74f, 0.20f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.50f, 0.11f, 0.11f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    const bool exitPressed = ImGui::Button("Exit Overlay", ImVec2(-1.0f, 0.0f));
    ImGui::PopStyleColor(4);
    return exitPressed;
}

bool drawOverlay(int width, int height) {
    if (!gConfig.menuExpanded) {
        return false;
    }

    ImGui::SetNextWindowPos(ImVec2(48.0f, 230.0f), ImGuiCond_FirstUseEver);
    const float maxWindowWidth = std::max(320.0f, static_cast<float>(width) - 96.0f);
    const float minWindowWidth = std::min(540.0f, maxWindowWidth);
    ImGui::SetNextWindowSizeConstraints(
            ImVec2(minWindowWidth, 0.0f),
            ImVec2(maxWindowWidth, std::max(360.0f, static_cast<float>(height) - 280.0f)));

    bool exitPressed = false;
    if (ImGui::Begin("Simple ImGui Termux", nullptr,
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoScrollbar)) {
        if (ImGui::BeginTabBar("MenuTabs")) {
            if (ImGui::BeginTabItem("Visual")) {
                drawVisualTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("ESP")) {
                drawEspTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Kontrol")) {
                drawControlTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("System")) {
                exitPressed = drawSystemTab(width, height);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    } else {
        ImGui::End();
    }
    return exitPressed;
}

void drawFeatures(int width, int height) {
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();

    if (gConfig.showCrosshair) {
        const ImVec2 center(static_cast<float>(width) * 0.5f,
                            static_cast<float>(height) * 0.5f);
        const float halfSize = gConfig.crosshairSize * 0.5f;
        const ImU32 color = IM_COL32(230, 230, 225, 220);

        drawList->AddLine(ImVec2(center.x - halfSize, center.y),
                          ImVec2(center.x + halfSize, center.y), color,
                          gConfig.crosshairThickness);
        drawList->AddLine(ImVec2(center.x, center.y - halfSize),
                          ImVec2(center.x, center.y + halfSize), color,
                          gConfig.crosshairThickness);
    }

    if (gConfig.enableEsp) {
        drawEsp(width, height);
    }
}

}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_SurfaceHost_nativeStart(JNIEnv* env, jclass, jobject surface, jint width, jint height) {
    LOGI("nativeStart %dx%d", width, height);
    gRunning.store(true);
    gSignalStop = 0;
    gTouchSeen.store(false);
    gTouchDown.store(false);

    if (surface == nullptr || width <= 0 || height <= 0) {
        LOGE("Invalid surface or display size");
        return;
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr) {
        LOGE("ANativeWindow_fromSurface returned null");
        return;
    }

    EglState egl;
    if (!initEgl(window, width, height, &egl)) {
        shutdownEgl(&egl);
        ANativeWindow_release(window);
        return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    configureStyle(io);
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = 1.0f / 60.0f;

    if (!ImGui_ImplOpenGL3_Init("#version 300 es")) {
        LOGE("ImGui OpenGL3 backend init failed");
        ImGui::DestroyContext();
        shutdownEgl(&egl);
        ANativeWindow_release(window);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(gImGuiMutex);
        gImGuiIo = &io;
        gImGuiIo->AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    TouchDevice touch = findTouchDevice();
    if (touch.fd < 0) {
        LOGW("No touchscreen with ABS_MT_POSITION_X/Y found");
    }

    std::thread inputThread;
    if (touch.fd >= 0) {
        try {
            inputThread = std::thread(touchLoop, touch, static_cast<int>(width),
                                      static_cast<int>(height));
        } catch (const std::exception& error) {
            LOGE("Could not start touch reader: %s", error.what());
            close(touch.fd);
        }
    }

    using Clock = std::chrono::steady_clock;
    auto previous = Clock::now();

    while (shouldRun()) {
        const auto now = Clock::now();
        const float delta = std::chrono::duration<float>(now - previous).count();
        previous = now;

        {
            std::lock_guard<std::mutex> lock(gImGuiMutex);
            io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
            io.DeltaTime = std::clamp(delta, 0.001f, 0.1f);
            ImGui_ImplOpenGL3_NewFrame();
            ImGui::NewFrame();
        }

        drawFloatingToggle();
        const bool exitPressed = drawOverlay(width, height);
        drawFeatures(width, height);
        if (exitPressed) {
            LOGI("Exit requested from UI");
            gRunning.store(false);
        }

        ImGui::Render();
        glViewport(0, 0, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (eglSwapBuffers(egl.display, egl.surface) != EGL_TRUE) {
            LOGE("eglSwapBuffers failed: 0x%x", eglGetError());
            break;
        }
    }

    gRunning.store(false);
    if (gSignalStop != 0) {
        LOGW("Stop requested by signal");
    }
    LOGI("Render loop stopped");
    if (inputThread.joinable()) {
        inputThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(gImGuiMutex);
        gImGuiIo = nullptr;
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();

    shutdownEgl(&egl);
    ANativeWindow_release(window);
}

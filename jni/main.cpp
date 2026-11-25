#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
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

std::atomic<bool> gRunning{true};
std::atomic<bool> gTouchReady{false};
std::atomic<bool> gTouchSeen{false};
std::atomic<bool> gTouchDown{false};
std::atomic<int> gTouchX{0};
std::atomic<int> gTouchY{0};
volatile sig_atomic_t gSignalStop = 0;

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

bool drawOverlay(int width, int height, bool* windowOpen) {
    ImGui::SetNextWindowPos(ImVec2(48.0f, 230.0f), ImGuiCond_FirstUseEver);
    const float maxWindowWidth = std::max(320.0f, static_cast<float>(width) - 96.0f);
    const float minWindowWidth = std::min(540.0f, maxWindowWidth);
    ImGui::SetNextWindowSizeConstraints(
            ImVec2(minWindowWidth, 0.0f),
            ImVec2(maxWindowWidth, std::max(360.0f, static_cast<float>(height) - 280.0f)));

    if (ImGui::Begin("Simple ImGui Termux", windowOpen,
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoScrollbar)) {
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

        ImGui::Separator();

        static bool testToggle = false;
        static float testSlider = 25.0f;
        ImGui::Checkbox("Test Toggle", &testToggle);
        ImGui::SetNextItemWidth(std::min(300.0f, static_cast<float>(width) * 0.32f));
        ImGui::SliderFloat("Test Slider", &testSlider, 0.0f, 100.0f, "%.1f");

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.63f, 0.16f, 0.16f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.74f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.50f, 0.11f, 0.11f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        const bool exitPressed = ImGui::Button("Exit Overlay", ImVec2(-1.0f, 0.0f));
        ImGui::PopStyleColor(4);

        ImGui::End();
        return exitPressed;
    }
    ImGui::End();
    return false;
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

        bool windowOpen = true;
        const bool exitPressed = drawOverlay(width, height, &windowOpen);
        if (exitPressed || !windowOpen) {
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

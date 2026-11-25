# simple-imgui

A standalone Dear ImGui overlay for rooted Android 14 devices. It runs through
`app_process` from Termux, without installing an APK.

## Requirements

- Android 14, arm64
- Root access through `su`
- Termux

## Download and run

```sh
curl -L -o simple_imgui.sh \
  https://github.com/CumiFork/simple-imgui/releases/latest/download/simple_imgui.sh
su -c "sh $PWD/simple_imgui.sh"
```

The script extracts the DEX and native library into
`/data/local/tmp/simple_imgui_run/`, starts the overlay, and removes the folder
when the process exits.

## Controls

- Drag the title bar to move the window.
- `Test Toggle` and `Test Slider` are live demo controls.
- `Exit Overlay` stops the render loop and closes the process.
- The status block shows measured FPS, display size, and the latest touch
  coordinate.

## How it works

- `java/SurfaceHost.java` creates a transparent `SurfaceControl` layer with
  public Android SDK APIs, then calls the JNI entry point.
- `jni/main.cpp` sets up EGL with an 8-bit alpha channel, renders with OpenGL
  ES 3.0, and reads touch events directly from `/dev/input/event*`.
- The launcher decodes a Base64 tar archive that contains `classes.dex` and
  `libsimple_imgui.so`, then passes the display size and library path to Java.

Touches are read from the kernel input device, so the overlay can work without
an Android window or input channel. The same tap can still reach the app
underneath, which is a limitation of this overlay method.

## Build

All compilation runs on GitHub Actions (`ubuntu-latest`):

1. Clone Dear ImGui `v1.91.5` into `jni/imgui/`.
2. Compile `java/SurfaceHost.java` with `javac` and `d8`.
3. Compile `libsimple_imgui.so` with the Android NDK for `arm64-v8a` and
   `android-29`.
4. Package both files as a tar.gz, Base64 encode the archive, and append it to
   `simple_imgui.sh`.

The workflow publishes the script as a workflow artifact and updates the
`latest` release.

## Layout

```
.github/workflows/build.yml   GitHub Actions pipeline
jni/main.cpp                  EGL, OpenGL ES, Dear ImGui, touch reader
jni/CMakeLists.txt            NDK build configuration
java/SurfaceHost.java         SurfaceControl host for app_process
scripts/launcher.sh.in        Source template for the standalone script
```

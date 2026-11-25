cmake_minimum_required(VERSION 3.22.1)
project(simple_imgui LANGUAGES CXX)

if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "This project targets arm64-v8a only. Pass -DANDROID_ABI=arm64-v8a.")
endif()
if(NOT ANDROID_PLATFORM STREQUAL "android-29")
    message(FATAL_ERROR "This project targets android-29. Pass -DANDROID_PLATFORM=android-29.")
endif()
if(NOT ANDROID_STL STREQUAL "c++_static")
    message(FATAL_ERROR "This project requires ANDROID_STL=c++_static.")
endif()

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/imgui")
if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR "Dear ImGui v1.91.5 is missing. Clone it into jni/imgui first.")
endif()

add_library(simple_imgui SHARED
        main.cpp
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)

target_compile_features(simple_imgui PRIVATE cxx_std_17)
target_compile_definitions(simple_imgui PRIVATE IMGUI_IMPL_OPENGL_ES3)
target_include_directories(simple_imgui PRIVATE
        "${IMGUI_DIR}"
        "${IMGUI_DIR}/backends")

target_link_libraries(simple_imgui
        EGL
        GLESv3
        android
        log)

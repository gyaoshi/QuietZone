cmake_minimum_required(VERSION 3.22.1)
project("ancapp")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Oboe 音频库
find_package(oboe REQUIRED CONFIG)

# NEON 优化
if(ANDROID_ABI STREQUAL "arm64-v8a" OR ANDROID_ABI STREQUAL "armeabi-v7a")
    set(NEON_SUPPORTED TRUE)
    add_definitions(-DUSE_NEON=1)
endif()

# 核心ANC库
add_library(anc_engine SHARED
        anc_engine.cpp
        fxlms.cpp
        secondary_path.cpp
        spectrum_analyzer.cpp
        audio_processor.cpp
        oboe_callback.cpp
)

target_include_directories(anc_engine PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(anc_engine PRIVATE
        -O3
        -ffast-math
        -fomit-frame-pointer
        -funroll-loops
        -Wall
        -Werror
)

if(NEON_SUPPORTED)
    if(ANDROID_ABI STREQUAL "armeabi-v7a")
        target_compile_options(anc_engine PRIVATE -mfpu=neon)
    endif()
    # arm64-v8a 默认支持 NEON, 无需额外 flag
endif()

target_link_libraries(anc_engine
        oboe::oboe
        android
        log
)

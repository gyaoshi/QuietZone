cmake_minimum_required(VERSION 3.16)
project(anc_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译选项
add_compile_options(-O2 -Wall -Wextra)

# 源文件 (排除 Android/JNI/Oboe 依赖的文件)
set(ANC_CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/../app/src/main/cpp/fxlms.cpp
    ${CMAKE_SOURCE_DIR}/../app/src/main/cpp/secondary_path.cpp
    ${CMAKE_SOURCE_DIR}/../app/src/main/cpp/spectrum_analyzer.cpp
    ${CMAKE_SOURCE_DIR}/../app/src/main/cpp/audio_processor.cpp
)

# 包含路径
set(ANC_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/../app/src/main/cpp)

# ============================================================
# 单元测试
# ============================================================
add_executable(anc_unit_test
    anc_unit_test.cpp
    ${ANC_CORE_SOURCES}
)
target_include_directories(anc_unit_test PRIVATE ${ANC_INCLUDE_DIR})
# 桌面不需要 NEON
target_compile_definitions(anc_unit_test PRIVATE)

# ============================================================
# 性能基准
# ============================================================
add_executable(anc_benchmark
    anc_benchmark.cpp
    ${ANC_CORE_SOURCES}
)
target_include_directories(anc_benchmark PRIVATE ${ANC_INCLUDE_DIR})
target_compile_definitions(anc_benchmark PRIVATE)
target_compile_options(anc_benchmark PRIVATE -O3 -ffast-math)

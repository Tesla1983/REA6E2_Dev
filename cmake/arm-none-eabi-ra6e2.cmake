# ============================================================
# Toolchain: Arm GNU Toolchain (arm-none-eabi)  for Renesas RA6E2
# 目标: Cortex-M33 (ARMv8-M, TrustZone, 单精度 FPU)
# 用法: cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-ra6e2.cmake
# ============================================================

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR cortex-m33)

# 裸机交叉编译: 跳过编译器自检(无法链接可执行文件)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# 直接在 PATH 里定位工具链(默认 E:\software\ArmGNUToolchain\14.3 rel1\bin)
find_program(CMAKE_C_COMPILER   NAMES arm-none-eabi-gcc   REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES arm-none-eabi-g++   REQUIRED)
find_program(CMAKE_ASM_COMPILER NAMES arm-none-eabi-gcc   REQUIRED)
find_program(CMAKE_AR           NAMES arm-none-eabi-ar    REQUIRED)
find_program(CMAKE_OBJCOPY      NAMES arm-none-eabi-objcopy REQUIRED)
find_program(CMAKE_OBJDUMP      NAMES arm-none-eabi-objdump REQUIRED)
find_program(CMAKE_SIZE         NAMES arm-none-eabi-size  REQUIRED)
find_program(CMAKE_GDB          NAMES arm-none-eabi-gdb)

message(STATUS "Toolchain : ${CMAKE_C_COMPILER}")

# ---- 架构相关编译选项 ------------------------------------
# 与官方 e²studio 工程(Debug/compile_commands.json)保持一致的机器选项
set(RA6E2_CPU_OPTIONS
    -mcpu=cortex-m33
    -mthumb
    -mfloat-abi=hard
    -mfpu=fpv5-sp-d16
)

add_compile_options(${RA6E2_CPU_OPTIONS})
add_link_options(${RA6E2_CPU_OPTIONS})

# ---- 工具链行为 ------------------------------------------
set(CMAKE_C_STANDARD   99)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)

# 不要自动追加 -rdynamic / 共享库相关标志
set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")
set(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "")
set(CMAKE_EXE_LINKER_FLAGS_INIT "")

# 搜索路径限制在工具链目录, 防止误用主机库
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

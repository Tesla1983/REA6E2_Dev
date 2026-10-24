# ============================================================
# 立创·地奇星 RA6E2 (R7FA6E2BB) 固件工程
#
#   cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-ra6e2.cmake
#   cmake --build build
#
# 常用目标:
#   cmake --build build           编译 (ELF/HEX/BIN/SREC)
#   cmake --build build -t size   查看占用
#   cmake --build build -t flash  烧录 (probe-rs + DAPLink)
#   cmake --build build -t probe  连通性自检
# ============================================================

cmake_minimum_required(VERSION 3.22)
project(ra6e2_fw VERSION 0.1.0 LANGUAGES C ASM)

# ------------------------------------------------------------
# 路径
# ------------------------------------------------------------
# 默认用 FSP 6.6.0(RASC 生成的工程, 已在板子上实测跑通)。
# 切回 5.8.0 官方示例: -DFSP_ROOT=.../vendor/DQX_RA6E2/BH1750
#   —— 注意两套的 ra_gen/ 是整体生成的, 不能混用。
set(FSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/projects/ra6e2_66_app"
    CACHE PATH "RASC 生成的 FSP 6.6.0 工程根(FSP / CMSIS / ra_gen / 链接脚本均取自此处)")
set(LD_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ld"
    CACHE PATH "链接脚本目录(fsp.ld + memory_regions.ld)")
set(APP_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/src")

# probe-rs 配置: 目标芯片 + 本项目专用探针
set(RA_CHIP      "R7FA6E2BB" CACHE STRING "probe-rs 目标芯片(256KB flash 变体)")
set(RA_PROBE_SEL "0d28:0204" CACHE STRING "本项目 DAPLink; FireDAP(0416:5021) 属 STM32H743, 禁止使用")
find_program(PROBE_RS NAMES probe-rs probe-rs.exe REQUIRED)

# 内存占用报告(scripts/size_report.py)需要 Python, 仅用标准库
find_program(PYTHON_EXE NAMES python3 python REQUIRED)

# ------------------------------------------------------------
# 源文件
# ------------------------------------------------------------
file(GLOB_RECURSE FSP_SOURCES   CONFIGURE_DEPENDS "${FSP_ROOT}/ra/fsp/src/*.c")
file(GLOB         FSP_GEN_SRC   CONFIGURE_DEPENDS "${FSP_ROOT}/ra_gen/*.c")
file(GLOB_RECURSE APP_SOURCES   CONFIGURE_DEPENDS "${APP_DIR}/*.c")

list(APPEND ALL_SOURCES ${FSP_SOURCES} ${FSP_GEN_SRC} ${APP_SOURCES})

# u8g2（开源单色图形库, BSD-2-Clause）: 只拷了核心绘图 + ST7565/NT7534 驱动 + 一个 7x13 字体,
# 没有拷 u8g2_fonts.c(39MB 全字库) 与 u8g2_d_setup.c(全部屏的 setup)。
# 链接开了 --gc-sections, 没用到的函数/字库不会进 flash。
set(U8G2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/u8g2")
file(GLOB U8G2_SOURCES CONFIGURE_DEPENDS "${U8G2_DIR}/*.c")
list(APPEND ALL_SOURCES ${U8G2_SOURCES})

if(NOT ALL_SOURCES)
    message(FATAL_ERROR "未找到源文件, 请检查 FSP_ROOT=${FSP_ROOT}")
endif()

add_executable(${PROJECT_NAME}.elf ${ALL_SOURCES})

# ------------------------------------------------------------
# 宏定义 (与 e²studio 工程一致)
# ------------------------------------------------------------
target_compile_definitions(${PROJECT_NAME}.elf PRIVATE
    _RENESAS_RA_
    _RA_CORE=CM33
    _RA_ORDINAL=1
    # 外部 20MHz 晶振(EXTAL/XTAL = P212/P213)要靠它才起振:
    # RASC 的 RA6E2 BSP 没有"主振驱动能力"选项, 生成的 bsp_clock_cfg.h 里既无
    # CGC_MAINCLOCK_DRIVE 也无 BSP_CFG_AUTODRVEN ⇒ MODRV 缺省 0(最低驱动), 带不动 20MHz。
    # 置 1 让硬件按频率自动选驱动(MODRV 被忽略)。放编译选项里, RASC 重新生成也不会丢。
    BSP_CFG_AUTODRVEN=1
)

# ------------------------------------------------------------
# 头文件搜索路径 (顺序照抄官方 compile_commands.json)
# ------------------------------------------------------------
target_include_directories(${PROJECT_NAME}.elf PRIVATE
    "${APP_DIR}"
    "${APP_DIR}/Apply"
    "${FSP_ROOT}/src"
    "${FSP_ROOT}/ra/fsp/inc"
    "${FSP_ROOT}/ra/fsp/inc/api"
    "${FSP_ROOT}/ra/fsp/inc/instances"
    "${FSP_ROOT}/ra/arm/CMSIS_6/CMSIS/Core/Include"
    "${FSP_ROOT}/ra_gen"
    "${FSP_ROOT}/ra_cfg/fsp_cfg/bsp"
    "${FSP_ROOT}/ra_cfg/fsp_cfg"
    # FSP 6.x 补丁目录: 提供 RASC 不生成的 bsp_mcu_ofs_cfg.h。
    # #include "..." 会先找 bsp_cfg.h 所在目录, 找不到再回落到这里的 -I 路径,
    # 5.8.0 不需要它(那份没有 OFS 机制), 加进来也无害。
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch66"
    # FSP 6.x: RASC 会把 bsp_linker_info.h 直接生成在工程根(不在 ra_cfg 下),
    # bsp_common.h 要 include 它, 所以工程根也要进搜索路径。
    # 放在最后, 优先级最低, 不会遮蔽上面任何目录里的同名头。
    "${FSP_ROOT}"
    # u8g2 头文件(u8g2.h / u8x8.h)。.c 与 .h 同目录, 源文件里的 #include "u8g2.h" 能直接命中。
    "${U8G2_DIR}"
)

# ------------------------------------------------------------
# 编译选项 (照抄 e²studio Debug 配置)
# ------------------------------------------------------------
target_compile_options(${PROJECT_NAME}.elf PRIVATE
    -Og                        # 调试友好; 发版可改 -O2 / -Os
    -g3
    -fmessage-length=0
    -fsigned-char
    -ffunction-sections
    -fdata-sections
    -fno-strict-aliasing
    --param=min-pagesize=0
    -Wall -Wextra -Wunused -Wuninitialized
    -Wpointer-arith -Wshadow -Wlogical-op -Wfloat-equal
    -Wmissing-declarations
)

# ------------------------------------------------------------
# 链接选项
# ------------------------------------------------------------
# 链接脚本定位:
#   FSP 6.x —— RASC 生成。判据是工程根存在 fsp_gen.ld(6.x 独有),
#              此时 fsp.ld 在 script/ 下且 INCLUDE 根目录的 memory_regions.ld + fsp_gen.ld
#   FSP 5.x —— 官方示例工程。判据是没有 fsp_gen.ld,
#              此时用 cmake/ld/ 下的 fsp.ld 副本(5.x 的 memory_regions.ld 在工程 Debug/ 里)
# 注意: 5.x 工程也有 script/fsp.ld, 不能拿它当判据。
# -L 必须写成 -Wl,-L<path> 这种"单参数"形式, 不能写成 -L <path>:
#   target_link_options() 会对选项做去重, 两个 "-L path1 -L path2" 里的
#   第二个 -L 会被当重复项丢掉, 剩下的裸路径变成 ld 的输入文件,
#   报 "cannot find <dir>: Permission denied"(在 Windows 上 open 目录即此错)。
#   走 -Wl, 后是单个参数, 不参与去重, 也就不会错位。
if(EXISTS "${FSP_ROOT}/fsp_gen.ld")
    set(RA_LD_SCRIPT "${FSP_ROOT}/script/fsp.ld")
    # 6.x 的 script/fsp.ld 里 INCLUDE 的是 memory_regions.ld 与 fsp_gen.ld,
    # 这两个都由 RASC 生成在【工程根】, 不是 script/ 下。
    set(RA_LD_SEARCH "-Wl,-L${FSP_ROOT}")
    set(RA_FSP_LAYOUT "6.x (RASC 生成)")
else()
    set(RA_LD_SCRIPT "${LD_DIR}/fsp.ld")
    set(RA_LD_SEARCH "-Wl,-L${LD_DIR}")
    set(RA_FSP_LAYOUT "5.x (cmake/ld 副本)")
endif()
message(STATUS " 链接脚本 : ${RA_LD_SCRIPT}  [${RA_FSP_LAYOUT}]")

target_link_options(${PROJECT_NAME}.elf PRIVATE
    -T "${RA_LD_SCRIPT}"
    ${RA_LD_SEARCH}
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_BINARY_DIR}/${PROJECT_NAME}.map
    # 原来的 -Wl,--print-memory-usage 已移除: 编译后由 scripts/size_report.py
    # 输出 ESP-IDF 风格的内存占用表(细分到段), 信息比它全, 见 cmake/PostBuild.cmake
    --specs=nano.specs
)

# ------------------------------------------------------------
# 构建后产物: hex / bin / srec + 体积报告
# ------------------------------------------------------------
set(ELF_FILE $<TARGET_FILE:${PROJECT_NAME}.elf>)

add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DOBJCOPY=${CMAKE_OBJCOPY}
            -DPYTHON_EXE=${PYTHON_EXE}
            -DELF=${ELF_FILE}
            -DOUTDIR=${CMAKE_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PostBuild.cmake
    COMMENT "生成 HEX/BIN/SREC 并输出体积"
    VERBATIM
)

# ------------------------------------------------------------
# 辅助目标
# ------------------------------------------------------------
add_custom_target(size
    COMMAND ${CMAKE_SIZE} --format=berkeley ${ELF_FILE}
    DEPENDS ${PROJECT_NAME}.elf
    COMMENT "查看 Flash/RAM 占用"
)

add_custom_target(probe
    COMMAND ${PROBE_RS} info --chip ${RA_CHIP} --probe ${RA_PROBE_SEL}
    COMMENT "DAPLink 连通性自检"
    VERBATIM
)

# ------------------------------------------------------------
# option-setting 段烧录策略(重要)
# ------------------------------------------------------------
# probe-rs 的 RA6E2 烧录算法不覆盖 option-setting 区(0x0100A100..0x0100A2FF),
# 直接 download 会报 "No flash memory contains the entire requested memory range"。
# 且该区域控制调试口锁定 / 看门狗 / HOCO 频率, 写错会锁死 SWD。
#
# 实测: 板上现有 OFS 与 e²studio 官方工程生成的值完全一致
#   OFS0(0x0100A100) = 全 0xFF;  SAS(0x0100A134) = 全 0xFF
#   OFS1(0x0100A200) = 0xFFFFFDFF —— 只有 HOCOFRQ 位(20MHz), 无调试锁定位
# 因此烧录时跳过这些段, 沿用芯片上已烧好的值。
set(RA_SKIP_SECTIONS
    .option_setting_ofs
    .option_setting_sas
    .option_setting_ns
    .option_setting_s
    .id_code
)

# 转成命令行形式: --skip-section <name> --skip-section <name> ...
set(RA_SKIP_ARGS "")
foreach(_sec IN LISTS RA_SKIP_SECTIONS)
    list(APPEND RA_SKIP_ARGS --skip-section "${_sec}")
endforeach()

# 烧录: 会擦除片上现有程序, 需手动显式调用
# 注意: download 的 --verify 是下载后顺带校验, 与独立的 verify 子命令不是一回事
add_custom_target(flash
    COMMAND ${PROBE_RS} download --chip ${RA_CHIP} --probe ${RA_PROBE_SEL}
            ${RA_SKIP_ARGS}
            ${ELF_FILE}
    DEPENDS ${PROJECT_NAME}.elf
    COMMENT "烧录 ELF 到 RA6E2 (将擦除板上现有程序; 跳过 option-setting 段)"
    VERBATIM
)

# 烧录后校验: 与片上内容逐段比对
add_custom_target(verify
    COMMAND ${PROBE_RS} verify --chip ${RA_CHIP} --probe ${RA_PROBE_SEL}
            ${RA_SKIP_ARGS}
            ${ELF_FILE}
    DEPENDS ${PROJECT_NAME}.elf
    COMMENT "校验片上内容与 ELF 是否一致"
    VERBATIM
)

# GDB 服务器(:1337), 不烧录; 另开终端用 arm-none-eabi-gdb 连接
# 注意: `probe-rs debug` 是 DAP server, 不能独立交互使用, 不要用它
add_custom_target(gdbserver
    COMMAND ${PROBE_RS} gdb --chip ${RA_CHIP} --probe ${RA_PROBE_SEL}
    COMMENT "启动 GDB 服务器 :1337 (不烧录)"
    VERBATIM
)

message(STATUS "-------------------------------------------")
message(STATUS " 芯片    : ${RA_CHIP}  (Cortex-M33, 256KB flash / 40KB SRAM)")
message(STATUS " 探针    : ${RA_PROBE_SEL} (DAPLink CMSIS-DAP v1)")
message(STATUS " FSP     : ${FSP_ROOT}")
message(STATUS " 源文件  : ${FSP_SOURCES}" )
message(STATUS "-------------------------------------------")

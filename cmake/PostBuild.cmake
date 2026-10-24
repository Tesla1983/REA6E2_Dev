# 构建后处理: 由 CMakeLists.txt 以 cmake -P 方式调用
#   入参: OBJCOPY, SIZE, ELF, OUTDIR
# 说明: 不用 add_custom_command 的多条 COMMAND, 避免 Windows 下
#       CMake 生成嵌套 `cmd.exe /C` 调用导致 "'cmd.exe' 不是内部或外部命令"。

set(BASENAME "ra6e2_fw")

message(STATUS "生成 HEX / BIN / SREC ...")
execute_process(
    COMMAND "${OBJCOPY}" -O ihex   "${ELF}" "${OUTDIR}/${BASENAME}.hex"
    RESULT_VARIABLE r1
)
execute_process(
    COMMAND "${OBJCOPY}" -O binary "${ELF}" "${OUTDIR}/${BASENAME}.bin"
    RESULT_VARIABLE r2
)
execute_process(
    COMMAND "${OBJCOPY}" -O srec   "${ELF}" "${OUTDIR}/${BASENAME}.srec"
    RESULT_VARIABLE r3
)

if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR NOT r3 EQUAL 0)
    message(FATAL_ERROR "objcopy 失败: hex=${r1} bin=${r2} srec=${r3}")
endif()

# 内存占用报告: ESP-IDF 风格表格(详见 scripts/size_report.py)
# objdump 与 objcopy 在同一个工具链 bin 目录里, 直接推导, 不用再传一个参数
get_filename_component(TOOLBIN_DIR "${OBJCOPY}" DIRECTORY)
set(OBJDUMP "${TOOLBIN_DIR}/arm-none-eabi-objdump.exe")
if(NOT EXISTS "${OBJDUMP}")
    message(FATAL_ERROR "找不到 objdump: ${OBJDUMP}")
endif()

execute_process(
    COMMAND "${PYTHON_EXE}" "${CMAKE_CURRENT_LIST_DIR}/../scripts/size_report.py"
            --elf "${ELF}"
            --bin "${OUTDIR}/${BASENAME}.bin"
            --objdump "${OBJDUMP}"
    RESULT_VARIABLE r4
    ENCODING UTF8
)

message(STATUS "产物位于: ${OUTDIR}")

cmake_minimum_required(VERSION 3.20.0)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aoia_zigbee C)
# These standard server implementations ship as source but are disabled in
# Nordic's default HA device set. Enable them in the ZBOSS interface, not just app.
target_compile_definitions(zboss INTERFACE
    ZB_ZCL_SUPPORT_CLUSTER_PM2_5_MEASUREMENT=1
    ZB_ZCL_SUPPORT_CLUSTER_ANALOG_INPUT=1)

# R23 4.2.2.5 omits SINGLE from analog reporting. Without this fix a stock
# ZHA Configure Reporting request for PM2.5 is parsed incorrectly.
set(zboss_src ${ZEPHYR_NCS_ZIGBEE_MODULE_DIR}/lib/zboss/src)
set(patched_dir ${CMAKE_CURRENT_BINARY_DIR}/zboss_float_reporting)
execute_process(COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/patch_zboss.py ${zboss_src} ${patched_dir}
    COMMAND_ERROR_IS_FATAL ANY)
get_target_property(zboss_sources zboss-sources SOURCES)
list(FILTER zboss_sources EXCLUDE REGEX "[/]zcl_(common|reporting)\\.c$")
set_property(TARGET zboss-sources PROPERTY SOURCES ${zboss_sources}
    ${patched_dir}/zcl_common.c ${patched_dir}/zcl_reporting.c)
target_include_directories(zboss-sources PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS scripts/patch_zboss.py)

target_sources(app PRIVATE src/main.c src/aoia_protocol.c src/purifier.c src/zigbee_device.c)
target_include_directories(app PRIVATE src)

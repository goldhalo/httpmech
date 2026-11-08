# =============================================================================
#  httpmech —— HTTP/1.1 协议胶水层（C++17 / 基于 netmech / 自带 picohttpparser）
#
#  独立可构建工程：
#      cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#      cmake --build build
#      cmake --install build          # 默认 Linux /usr/local、Windows C:/Program Files/httpmech
#
#  依赖策略：
#      * TCP 传输用 netmech（同级目录 ../netmech，或 -DHTTPMECH_NETMECH_DIR=<路径>）
#      * 报文解析用 vendored 的 picohttpparser（thirdparty/，MIT）
#      * 不引入 TLS / 不引入任何需要额外安装的库
#
#  安装约定（与 netmech / modbusmech / rtspmech / rtpmech / mpegmech 一致）：
#      一个包只装自己，依赖由 Config.cmake 里的 find_dependency 声明、
#      由使用者自己装。
# =============================================================================
cmake_minimum_required(VERSION 3.16)

project(httpmech
        VERSION 0.1.0
        DESCRIPTION "httpmech HTTP/1.1 protocol glue built on netmech"
        LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# 安装目录规范。必须早于 target 定义：$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
# 里的变量是立即展开的，晚定义会展开成空串。
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# 1. 输出目录
# ---------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ---------------------------------------------------------------------------
# 2. 选项
# ---------------------------------------------------------------------------
option(HTTPMECH_BUILD_TESTS      "构建单元测试" ON)
option(HTTPMECH_BUILD_EXAMPLES   "构建示例（echo server / client）" ON)
option(HTTPMECH_WARNINGS_AS_ERRORS "把编译告警当错误（CI 用）" OFF)

# 位置无关代码：静态库常被链进 .so，缺 -fPIC 只会在最终链接那一步炸。
option(HTTPMECH_PIC "编译为位置无关代码（静态库要链进 .so 时必须开）" ON)

# ---------------------------------------------------------------------------
# 3. 定位 netmech
# ---------------------------------------------------------------------------
set(HTTPMECH_NETMECH_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../netmech"
    CACHE PATH "netmech 所在目录")

if(NOT EXISTS "${HTTPMECH_NETMECH_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "找不到 netmech：${HTTPMECH_NETMECH_DIR}\n"
        "   指定：cmake -DHTTPMECH_NETMECH_DIR=<netmech 路径> ...")
endif()

# 库类型：STATIC（默认）/ SHARED。
#
# ★ 必须在 add_subdirectory(netmech) 之前定义：SHARED 构建要求 netmech 也
#   以共享库提供 —— 只静态链 netmech 会让注入的 Runtime 跨两份副本使用
#   （netmech 文档明确禁止，rtspmech 里实测段错误）。
set(HTTPMECH_LIBRARY_TYPE "STATIC" CACHE STRING "库类型：STATIC 或 SHARED")
set_property(CACHE HTTPMECH_LIBRARY_TYPE PROPERTY STRINGS STATIC SHARED)
if(HTTPMECH_LIBRARY_TYPE STREQUAL "SHARED")
    set(NETMECH_LIBRARY_TYPE "SHARED" CACHE STRING "netmech 库类型" FORCE)
endif()

set(NETMECH_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(NETMECH_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# EXCLUDE_FROM_ALL：netmech 只参与编译，不参与本仓的安装（同其它 mech 库）
add_subdirectory("${HTTPMECH_NETMECH_DIR}" "${CMAKE_BINARY_DIR}/netmech"
                 EXCLUDE_FROM_ALL)

message(STATUS "httpmech: netmech @ ${HTTPMECH_NETMECH_DIR}")

# ---------------------------------------------------------------------------
# 4. 编译选项
# ---------------------------------------------------------------------------
add_library(httpmech_compile_options INTERFACE)
# 注意：genex 里的多个选项必须用 ';' 分隔，不能靠空格 ——
#   写成 "a b" 时，Makefile/Ninja 生成器把选项拼成命令行会"恰好"变成两个参数，
#   但 VS 生成器是按字符串原样塞进 AdditionalOptions，行为不一致。
target_compile_options(httpmech_compile_options INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:$<$<CXX_COMPILER_ID:MSVC>:/utf-8;/W4;/permissive-;/Zc:__cplusplus>>
    $<$<COMPILE_LANGUAGE:CXX>:$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra>>
    $<$<COMPILE_LANGUAGE:C>:$<$<C_COMPILER_ID:MSVC>:/utf-8;/W3>>
    $<$<COMPILE_LANGUAGE:C>:$<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall;-Wextra>>)
if(HTTPMECH_WARNINGS_AS_ERRORS)
    target_compile_options(httpmech_compile_options INTERFACE
        $<$<CXX_COMPILER_ID:MSVC>:/WX>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Werror>)
endif()

# ---------------------------------------------------------------------------
# 5. picohttpparser（vendored，编进本库）
# ---------------------------------------------------------------------------
#  它**只**是 parser 的实现细节，所以不再单独出 `libpicohttpparser.a`：
#     · 发布物只有 libhttpmech.a 一个库文件；
#     · 公开头目录里不出现 picohttpparser.h；
#     · 符号经 src/pico_rename.hpp 加了 httpmech_ 前缀，和 rtspmech 各自内嵌
#       一份也不会在链接期撞车。
#  源码由 src/pico_impl.cpp 以 C++ 方式编入（见该文件的说明）。

# ---------------------------------------------------------------------------
# 6. 主库
# ---------------------------------------------------------------------------
#  头文件先于实现落地。
#  构建成 INTERFACE 库，装了也能被 find_package 找到、能编译 —— 只是链接时
#  会缺符号，这是刻意的：接口评审阶段就该暴露"还没有实现"。
file(GLOB_RECURSE HTTPMECH_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

if(HTTPMECH_SOURCES)
    add_library(httpmech ${HTTPMECH_LIBRARY_TYPE} ${HTTPMECH_SOURCES})
    set(HTTPMECH_IS_INTERFACE OFF)
else()
    message(STATUS "httpmech: src/ 下暂无 .cpp，构建为 INTERFACE（header-only）库")
    add_library(httpmech INTERFACE)
    set(HTTPMECH_IS_INTERFACE ON)
endif()

add_library(httpmech::httpmech ALIAS httpmech)

if(HTTPMECH_LIBRARY_TYPE STREQUAL "SHARED")
    target_compile_definitions(httpmech PUBLIC HTTPMECH_SHARED)
    target_compile_definitions(httpmech PRIVATE HTTPMECH_BUILDING_LIBRARY)
    set_target_properties(httpmech PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    if(MSVC)
        target_compile_options(httpmech PRIVATE /wd4251)
    elseif(UNIX AND NOT APPLE)
        target_link_options(httpmech PRIVATE "-Wl,--exclude-libs,ALL")
    elseif(APPLE)
        target_link_options(httpmech PRIVATE "-Wl,-hidden-lsymbols")
    endif()
endif()
set_target_properties(httpmech PROPERTIES
    POSITION_INDEPENDENT_CODE ${HTTPMECH_PIC}
    FOLDER "httpmech")

# INTERFACE 库只能用 INTERFACE 关键字挂 include/link（PUBLIC 会报
# "may only be used with INTERFACE libraries"），所以两种形态分开写。
if(NOT HTTPMECH_IS_INTERFACE)
    target_include_directories(httpmech PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

    target_link_libraries(httpmech
        PUBLIC  netmech
        PRIVATE httpmech_compile_options)

    # pico 的头只在本库内部可见（parser.cpp 用 <picohttpparser.h>）
    target_include_directories(httpmech PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/picohttpparser")

    # 第三方代码不参与"告警当错误"：语法是上游的，改不动也不该改。
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/pico_impl.cpp"
        PROPERTIES COMPILE_OPTIONS
        "$<$<CXX_COMPILER_ID:MSVC>:/w>$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>")
else()
    target_include_directories(httpmech INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

    target_link_libraries(httpmech INTERFACE
        netmech httpmech_compile_options)
endif()

# ---------------------------------------------------------------------------
# 7. 测试 / 示例
# ---------------------------------------------------------------------------
if(HTTPMECH_BUILD_TESTS)
    enable_testing()
    file(GLOB HTTPMECH_TEST_SOURCES CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/test/*.cpp")
    if(HTTPMECH_TEST_SOURCES)
        add_executable(httpmech_ut ${HTTPMECH_TEST_SOURCES})
        target_link_libraries(httpmech_ut PRIVATE httpmech httpmech_compile_options)
        add_test(NAME httpmech_ut COMMAND httpmech_ut)
    endif()
endif()

if(HTTPMECH_BUILD_EXAMPLES)
    file(GLOB HTTPMECH_EXAMPLE_SOURCES CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/example/*.cpp")
    foreach(src IN LISTS HTTPMECH_EXAMPLE_SOURCES)
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE httpmech httpmech_compile_options)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# 8. 安装与导出
# ---------------------------------------------------------------------------
#  装完之后使用者只需要：
#      find_package(httpmech REQUIRED)
#      target_link_libraries(app PRIVATE httpmech::httpmech)
#  netmech 由 Config 里的 find_dependency 找；picohttpparser 已编进本库，
#  **不再作为独立的库随包安装**（使用方只需要链 libhttpmech.a 一个文件）。
if(NOT HTTPMECH_IS_INTERFACE)
    install(TARGETS httpmech
            EXPORT httpmechTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
    install(TARGETS httpmech EXPORT httpmechTargets)
endif()

install(TARGETS httpmech_compile_options
        EXPORT httpmechTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(DIRECTORY include/httpmech
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")

# picohttpparser 的头**不装**：它已被编进 libhttpmech.a，属于实现细节。
# 但 MIT 要求分发时保留版权声明，所以把它的许可证随文档一起装。
install(FILES thirdparty/picohttpparser/LICENSE
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/httpmech
        RENAME LICENSE.picohttpparser)

install(EXPORT httpmechTargets
        FILE httpmechTargets.cmake
        NAMESPACE httpmech::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/httpmech)

configure_package_config_file(cmake/httpmechConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/httpmechConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/httpmech)

write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/httpmechConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/httpmechConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/httpmechConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/httpmech)

install(FILES LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/httpmech)

# ---------------------------------------------------------------------------
# 9. 摘要
# ---------------------------------------------------------------------------
message(STATUS "----------------------------------------------------------")
message(STATUS "  httpmech ${PROJECT_VERSION}")
message(STATUS "    C++ 标准      : ${CMAKE_CXX_STANDARD}")
message(STATUS "    构建类型      : ${CMAKE_BUILD_TYPE}")
message(STATUS "    netmech       : ${HTTPMECH_NETMECH_DIR}")
message(STATUS "    输出目录      : ${CMAKE_BINARY_DIR}")
message(STATUS "----------------------------------------------------------")

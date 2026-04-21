cmake_minimum_required(VERSION 3.16)
project(cinder VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Default to Release when no build type is specified
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ── Compiler flags ─────────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W3 /D_CRT_SECURE_NO_WARNINGS
        $<$<CONFIG:Release>:/O2 /DNDEBUG>
        $<$<CONFIG:Debug>:/Od /Zi>
    )
else()
    # Warnings applied to all targets/configs
    add_compile_options(-Wall -Wextra)
endif()

# ── Platform defines ───────────────────────────────────────────────────────────
if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN)
endif()

# ── Find system dependencies ───────────────────────────────────────────────────
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)

# ── QuickJS ────────────────────────────────────────────────────────────────────
set(QJS_DIR ${CMAKE_SOURCE_DIR}/third_party/quickjs)
set(QJS_SOURCES
    ${QJS_DIR}/quickjs.c
    ${QJS_DIR}/dtoa.c
    ${QJS_DIR}/libregexp.c
    ${QJS_DIR}/libunicode.c
    ${QJS_DIR}/cutils.c
    ${QJS_DIR}/quickjs-libc.c
)

# Read VERSION
file(READ ${QJS_DIR}/VERSION QJS_VERSION)
string(STRIP "${QJS_VERSION}" QJS_VERSION)

add_library(quickjs STATIC ${QJS_SOURCES})
target_include_directories(quickjs PUBLIC ${QJS_DIR})
target_compile_definitions(quickjs PRIVATE
    CONFIG_VERSION="${QJS_VERSION}"
    _GNU_SOURCE
)
if(NOT MSVC)
    target_compile_options(quickjs PRIVATE -Wno-unused-parameter -Wno-sign-compare)
endif()

# ── cJSON ──────────────────────────────────────────────────────────────────────
set(CJSON_DIR ${CMAKE_SOURCE_DIR}/third_party/cjson)
add_library(cjson STATIC ${CJSON_DIR}/cJSON.c)
target_include_directories(cjson PUBLIC ${CJSON_DIR})

# ── Cinder sources ─────────────────────────────────────────────────────────────
set(CINDER_SOURCES
    src/main.c
    src/cli/cli.c
    src/runtime/runtime.c
    src/runtime/modules/mod_fs.c
    src/runtime/modules/mod_path.c
    src/runtime/modules/mod_process.c
    src/runtime/modules/mod_require.c
    src/pm/pm.c
    src/pm/registry.c
    src/pm/resolver.c
    src/pm/installer.c
    src/pm/lockfile.c
    src/dev/devserver.c
)

add_executable(cinder ${CINDER_SOURCES})

target_compile_definitions(cinder PRIVATE _GNU_SOURCE)

# Per-config optimization flags only for the cinder target (not vendored libs)
if(NOT MSVC)
    target_compile_options(cinder PRIVATE
        $<$<CONFIG:Release>:-O3 -march=native -DNDEBUG>
        $<$<CONFIG:Debug>:-O0 -g>
    )
    # LTO only for cinder (not QuickJS — can be fragile)
    set_property(TARGET cinder PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

target_compile_definitions(cinder PRIVATE _GNU_SOURCE)

target_include_directories(cinder PRIVATE
    src
    src/util
    src/dev
    ${QJS_DIR}
    ${CJSON_DIR}
)

target_link_libraries(cinder PRIVATE
    quickjs
    cjson
    CURL::libcurl
    ZLIB::ZLIB
)

if(WIN32)
    target_link_libraries(cinder PRIVATE ws2_32 mswsock bcrypt)
elseif(UNIX)
    target_link_libraries(cinder PRIVATE m pthread dl)
endif()

# ── Install ────────────────────────────────────────────────────────────────────
install(TARGETS cinder RUNTIME DESTINATION bin)

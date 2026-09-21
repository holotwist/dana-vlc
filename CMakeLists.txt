cmake_minimum_required(VERSION 3.16)
project(dana-vlc VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
add_compile_options(-O3 -flto -Wall -Wextra)
add_compile_definitions(NDEBUG MODULE_STRING="dana" __PLUGIN__)

find_package(PkgConfig REQUIRED)
pkg_check_modules(VLC REQUIRED vlc-plugin>=3.0.0)

set(DANA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../dana" CACHE PATH "Path to DANA repository root")

if(NOT EXISTS "${DANA_SOURCE_DIR}/src/DANADecoder.c")
    message(FATAL_ERROR "Could not find DANA sources at ${DANA_SOURCE_DIR}. Use -DDANA_SOURCE_DIR=<path>")
endif()

message(STATUS "Using DANA sources from: ${DANA_SOURCE_DIR}")

# VLC module must be a MODULE library named <name>_plugin
add_library(dana_plugin MODULE
    src/vlc_dana.c
    ${DANA_SOURCE_DIR}/src/DANADecoder.c
    ${DANA_SOURCE_DIR}/src/DANAPredictor.c
    ${DANA_SOURCE_DIR}/src/DANACoder.c
    ${DANA_SOURCE_DIR}/src/DANABitStream.c
    ${DANA_SOURCE_DIR}/src/DANAUtility.c
)

set_target_properties(dana_plugin PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "dana_plugin"
)

target_include_directories(dana_plugin PRIVATE
    ${VLC_INCLUDE_DIRS}
    ${DANA_SOURCE_DIR}/src/include/public
    ${DANA_SOURCE_DIR}/src/include/private
)

target_compile_options(dana_plugin PRIVATE ${VLC_CFLAGS_OTHER})
target_link_libraries(dana_plugin PRIVATE ${VLC_LIBRARIES} m)

# Determine default VLC plugin dir from pkg-config
pkg_get_variable(VLC_PLUGIN_DIR vlc-plugin pluginsdir)
if(NOT VLC_PLUGIN_DIR)
    set(VLC_PLUGIN_DIR "${CMAKE_INSTALL_PREFIX}/lib/vlc/plugins")
endif()

install(TARGETS dana_plugin
    DESTINATION "${VLC_PLUGIN_DIR}/codec"
)
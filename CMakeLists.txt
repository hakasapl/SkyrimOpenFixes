cmake_minimum_required(VERSION 3.21)

set(VCPKG_TARGET_TRIPLET "x64-windows-static")

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

set(PLUGIN_NAME "OpenFixes")
set(PLUGIN_VERSION "0.1.0")
project(${PLUGIN_NAME} VERSION ${PLUGIN_VERSION} LANGUAGES CXX)

if(MSVC)
    # Match vcpkg's x64-windows-static triplet (/MT, /MTd) across all targets.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()

add_compile_definitions(PLUGIN_NAME="${PLUGIN_NAME}")
add_compile_definitions(PLUGIN_VERSION="${PLUGIN_VERSION}")

# CommonLibSSE-NG resolves game addresses and struct layouts at runtime, so one DLL serves
# every supported runtime. SE and AE are both enabled - AE here covers the 1.7.x releases
# (1.7.99 and 1.7.104, which need Address Library format 5 and SKSE 2.3.x) as well as the
# older 1.6.x ones. VR (1.4.15) is enabled too, so the same DLL loads under SKSEVR; each fix
# still only installs on the runtimes it lists in its FixInfo.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)
add_subdirectory("extern/CommonLibSSE-NG" commonlibsse)

find_package(safetyhook CONFIG REQUIRED)

# Every fix lives in its own folder under src/Fixes and registers itself, so new fixes only need a re-configure
file(GLOB_RECURSE headers CONFIGURE_DEPENDS "include/*.h" "src/*.h")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "src/*.cpp")

add_library(${PROJECT_NAME} SHARED ${headers} ${sources})

add_compile_definitions(NOMINMAX)
add_compile_definitions(WIN32_LEAN_AND_MEAN)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE "include/PCH.h")

target_include_directories(${PROJECT_NAME} PRIVATE "include" "src")

if(MSVC)
    # /bigobj: some fixes generate large amounts of Xbyak code; /utf-8: sources are UTF-8
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /utf-8)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE safetyhook::safetyhook)

if(DEFINED OUTPUT_FOLDER)
    set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")

    message(STATUS "SKSE plugin output folder: ${DLL_FOLDER}")

    add_custom_command(
        TARGET "${PROJECT_NAME}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        VERBATIM
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_custom_command(
            TARGET "${PROJECT_NAME}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
            VERBATIM
        )
    endif()
endif()

# eden-ps4: the PS4 frontend executable (included from inject.cmake after Eden's targets exist).
set(_fe "${EDEN_PS4_FRONTEND_DIR}")
add_executable(eden-ps4 "${_fe}/main.cpp" "${_fe}/ps4_platform.cpp" "${_fe}/ps4_platform.h"
    "${_fe}/heap_census.cpp" "${_fe}/menu/rom_menu.cpp" "${_fe}/menu/rom_menu.h"
    "${_fe}/../tests/flat_map_check.cpp"
    "${_fe}/libc_wide_fixes.cpp"
    # compiler-rt 18.1.8 emulated-TLS runtime (the whole build uses -femulated-tls)
    "${_fe}/emutls/emutls.c")
target_include_directories(eden-ps4 PRIVATE "${PROJECT_SOURCE_DIR}/src" "${_fe}")
target_link_libraries(eden-ps4 PRIVATE common core audio_core video_core input_common hid_core
    frontend_common Vulkan::Headers GPUOpen::VulkanMemoryAllocator)
# Eden is built without RTTI; the window class derives from one of its classes.
target_compile_options(eden-ps4 PRIVATE -fno-rtti)

# The upstream scm_rev can stay cached across incremental builds. Give each configured test
# its own identity and retain it beside the binary, so boot.log can be paired with its ELF.
string(TIMESTAMP _ps4_build_stamp "%Y%m%dT%H%M%SZ" UTC)
execute_process(COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE _ps4_eden_revision OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
set(_ps4_build_id "test29-${_ps4_build_stamp}-${_ps4_eden_revision}")
target_compile_definitions(eden-ps4 PRIVATE EDEN_PS4_BUILD_ID="${_ps4_build_id}")
file(WRITE "${CMAKE_BINARY_DIR}/eden-ps4-build-id.txt" "${_ps4_build_id}\n")

# RADV, statically linked through orbis-compat's Vulkan loader shim (its vkloader/CMakeLists.txt,
# inlined: deferred code cannot add a subdirectory).
set(_vkl "${ORBIS_COMPAT_DIR}/vkloader")
add_library(ps4-vkloader STATIC "${_vkl}/vkloader.c" "${_vkl}/vkthunks.c")
# PRIVATE: Eden compiles against its own Vulkan headers.
target_include_directories(ps4-vkloader PRIVATE "${_vkl}" "${ORBIS_MESA_SRC}/include")
file(GLOB _orbis_zlib "${ORBIS_MESA_BUILD}/subprojects/zlib-*/libz.a")
target_link_libraries(ps4-vkloader INTERFACE
    -Wl,--whole-archive "${ORBIS_MESA_BUILD}/src/amd/vulkan/libvulkan_radeon.a" -Wl,--no-whole-archive
    ${_orbis_zlib} -lSceGnmDriver -lSceVideoOut)
target_link_libraries(eden-ps4 PRIVATE ps4-vkloader)

# System libraries the frontend and the audio sink call.
target_link_libraries(eden-ps4 PRIVATE -lSceAudioOut -lScePad -lSceUserService -lSceSystemService
    -lSceSysmodule -lSceNet)

# Heap census (heap_census.cpp): every allocation goes through the wrappers.
target_link_options(eden-ps4 PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc
    -Wl,--wrap=free -Wl,--wrap=posix_memalign -Wl,--wrap=aligned_alloc)

if(COMMAND ps4_create_eboot)
    ps4_create_eboot(eden-ps4)
endif()

# The boost::unordered_flat_map check also builds as a PC program (tests/); here it runs at startup.
set_source_files_properties("${_fe}/../tests/flat_map_check.cpp" PROPERTIES COMPILE_DEFINITIONS EDEN_PS4_NO_MAIN)
# The console's libc does not search 4-byte wchar_t units; these replace its wide memory functions.
set_source_files_properties("${_fe}/libc_wide_fixes.cpp" PROPERTIES COMPILE_OPTIONS "-fno-builtin")

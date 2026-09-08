cmake_minimum_required(VERSION 3.18)

get_filename_component(PROJECT_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
project(${PROJECT_NAME} C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Toolchain paths
set(OO_TOOLCHAIN $ENV{OO_PS4_TOOLCHAIN})

if(NOT OO_TOOLCHAIN)
    message(FATAL_ERROR "OO_PS4_TOOLCHAIN environment variable not set")
endif()

# ---------------------------------------------------
# mbedTLS static library (identity crypto only)
# ---------------------------------------------------
# Compiles the mbedTLS library sources with the project-owned PS4 config
# (config/mbedtls_ps4_config.h). The config gates which features are compiled
# in; only what PairingStore needs (entropy/CTR_DRBG, bignum, RSA, PK,
# X.509 create/write PEM, SHA-256) is enabled. TLS/SSL is disabled.
file(GLOB MBEDTLS_SRC CONFIGURE_DEPENDS
    "third_party/mbedtls/library/*.c"
)

add_library(mbedtls_ps4 STATIC ${MBEDTLS_SRC})

target_compile_definitions(mbedtls_ps4 PRIVATE
    _GNU_SOURCE
    MBEDTLS_CONFIG_FILE="mbedtls_3.6.7_ps4_config.h"
)

target_include_directories(mbedtls_ps4 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/include
)

# ---------------------------------------------------
# moonlight-common-c (with enet + nanors submodules)
# ---------------------------------------------------
# Uses moonlight-common-c's own CMakeLists, which builds the enet submodule and
# compiles the nanors sources into the library automatically. Nothing under
# third_party/ is modified.
#
# USE_MBEDTLS=ON selects the mbedTLS crypto backend (PSA crypto, enabled in
# config/mbedtls_ps4_config.h). BUILD_SHARED_LIBS=OFF builds it static.
#
# moonlight-common-c's USE_MBEDTLS branch links a target named "mbedcrypto";
# alias our mbedtls_ps4 target to that name before adding the subdirectory.
add_library(mbedcrypto ALIAS mbedtls_ps4)

set(USE_MBEDTLS ON CACHE BOOL "Use MbedTLS instead of OpenSSL" FORCE)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(third_party/moonlight-common-c)

# moonlight-common-c's PlatformCrypto.c needs <psa/crypto.h> from mbedtls.
target_include_directories(moonlight-common-c SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/include
)

# Neutralize the upstream -Werror (OpenOrbis clang flags warnings as errors).
target_compile_options(moonlight-common-c PRIVATE -Wno-error)

# Replace moonlight-common-c's PlatformSockets.c with a project-owned PS4
# implementation that uses the native sceNet* API. The OpenOrbis POSIX socket
# wrappers are unreliable for UDP unicast reception on an ephemeral socket
# (see DiscoveryManager.cpp). Nothing under third_party/ is modified.
#
# The upstream PlatformSockets.c was added via aux_source_directory() in the
# moonlight-common-c subdirectory, so we cannot remove it with target_sources
# REMOVE. Instead we mark it HEADER_FILE_ONLY (so it is not compiled) and add
# our replacement source.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/moonlight-common-c/src/PlatformSockets.c
    TARGET_DIRECTORY moonlight-common-c
    PROPERTIES HEADER_FILE_ONLY ON
)
target_sources(moonlight-common-c PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/_slipstream/PlatformSockets.c
)

# Replace enet's unix.c with a project-owned PS4 implementation that uses the
# native sceNet* API. The OpenOrbis POSIX socket wrappers (bind/socket/sendmsg/
# recvmsg) are unreliable (see DiscoveryManager.cpp). Nothing under third_party/
# is modified.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/moonlight-common-c/enet/unix.c
    TARGET_DIRECTORY enet
    PROPERTIES HEADER_FILE_ONLY ON
)
target_sources(enet PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/_slipstream/unix.c
)
# The ported enet-unix.c includes "Limelight-internal.h" (from moonlight-common-c).
target_include_directories(enet PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/moonlight-common-c/src
)

# ---------------------------------------------------
# opus (audio codec)
# ---------------------------------------------------
# Uses opus's own CMakeLists, which compiles the CELT and SILK codec sources
# into the library automatically. Nothing under third_party/ is modified.
#
# OPUS_BUILD_TESTING=OFF and OPUS_BUILD_PROGRAMS=OFF disable the test and
# demo executables. OPUS_STACK_PROTECTOR=OFF avoids the -fstack-protector
# flag (the OpenOrbis toolchain does not provide libssp).
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/opus)

# opus's opus_decoder.c emits a #pragma-message warning when compiled without
# optimization ("opus will be very slow"). Add optimization to the opus target
# only, so the warning is silenced without switching the whole project to
# Release mode (which exposed an unrelated overlay-composition bug). The
# OpenOrbis clang toolchain flags warnings as errors, so this is required.
target_compile_options(opus PRIVATE -O2)

# ---------------------------------------------------
# h264bitstream (SPS rewrite for the H.264 decoder)
# ---------------------------------------------------
# Parses and re-serializes the H.264 Sequence Parameter Set so the decoder
# can be forced to a known-good reference-frame configuration (num_ref_frames
# = 1). Only the NAL/SEI/stream parsing units are needed for the SPS fixup;
# the slice-data and avcc units are not used. We skip h264bitstream's own
# CMakeLists (it uses FILE_SET / cmake 3.23 features) and build it directly.
add_library(h264bitstream STATIC
    third_party/h264bitstream/h264_nal.c
    third_party/h264bitstream/h264_sei.c
    third_party/h264bitstream/h264_stream.c
)
target_include_directories(h264bitstream PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/h264bitstream
)
# The OpenOrbis clang toolchain flags warnings as errors; h264bitstream has
# unused-parameter / sign-compare warnings we do not want to fix upstream.
target_compile_options(h264bitstream PRIVATE -Wno-unused-parameter -Wno-sign-compare)

# ---------------------------------------------------
# PS4 client source
# ---------------------------------------------------

file(GLOB_RECURSE CLIENT_SRC CONFIGURE_DEPENDS
    "src/*.c"
    "src/*.cpp"
)

add_executable(${PROJECT_NAME}.elf ${CLIENT_SRC})

# Centralize the _GNU_SOURCE workaround. The OpenOrbis libc++ headers
# (<atomic>/<__threading_support>) reference nanosleep without declaring it;
# the OpenOrbis <time.h> only declares nanosleep when a POSIX/GNU feature-test
# macro is defined. Defining _GNU_SOURCE at the target level resolves this for
# every translation unit (including the new threaded DiscoveryManager).
target_compile_definitions(${PROJECT_NAME}.elf PRIVATE _GNU_SOURCE)

# GPU video renderer production path.
# renderFrame() uses the real video_p.sb NV12→BGRA shader and binds the decoded
# Videodec2 NV12 planes (Onion WB, GPU-readable) directly as textures — no
# intermediate copy.
#
# GPU in-stream overlay (Stage 2): the real overlay compositor content is
# staged into a WC_GARLIC buffer on change and GPU-blended over the video in a
# single submission (renderFrameWithOverlay). No test macro is defined; the
# production path is active.

target_link_libraries(${PROJECT_NAME}.elf
    moonlight-common-c
    mbedtls_ps4
    opus
    h264bitstream
    opengnm
    SceVideoOut
    SceGnmDriver
    SceUserService
    SceAudioOut
    ScePad
    SceSysmodule
    SceFreeType
    SceNet
    SceNetCtl
    SceHttp
    SceSsl
    SceRandom
    SceIme
    SceImeDialog
    SceSystemService
    SceMouse
    SceConvertKeycode
    c
    c++
    kernel
)

target_include_directories(${PROJECT_NAME}.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/moonlight-common-c/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/h264bitstream
    ${OO_TOOLCHAIN}/include
    ${OO_TOOLCHAIN}/include/opengnm
)


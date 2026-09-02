set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(OO_TOOLCHAIN $ENV{OO_PS4_TOOLCHAIN})
if(NOT OO_TOOLCHAIN)
    message(FATAL_ERROR "OO_PS4_TOOLCHAIN environment variable not set")
endif()

# host tools inside your container
set(CMAKE_C_COMPILER /opt/homebrew/opt/llvm/bin/clang)
set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm/bin/clang++)
set(CMAKE_LINKER /opt/homebrew/opt/lld/bin/ld.lld)
set(CMAKE_AR /opt/homebrew/opt/llvm/bin/llvm-ar)
set(CMAKE_RANLIB /opt/homebrew/opt/llvm/bin/llvm-ranlib)

# Match the old Makefile compile flags
set(CMAKE_C_FLAGS_INIT
    "--target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -D__ORBIS__ -isysroot ${OO_TOOLCHAIN} -isystem ${OO_TOOLCHAIN}/include"
)

set(CMAKE_CXX_FLAGS_INIT
    "${CMAKE_C_FLAGS_INIT} -isystem ${OO_TOOLCHAIN}/include/c++/v1"
)

# Important: do NOT let CMake inject normal system libs/startfiles
set(CMAKE_C_STANDARD_LIBRARIES "")
set(CMAKE_CXX_STANDARD_LIBRARIES "")
set(CMAKE_EXE_LINKER_FLAGS_INIT "")

# Make CMake link executables the same way your old Makefile did
set(CMAKE_C_LINK_EXECUTABLE
    "${CMAKE_LINKER} <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> -m elf_x86_64 -pie --script ${OO_TOOLCHAIN}/link.x --eh-frame-hdr -L${OO_TOOLCHAIN}/lib -L${CMAKE_SOURCE_DIR}/lib ${OO_TOOLCHAIN}/lib/crt1.o --start-group <LINK_LIBRARIES> --end-group"
)
set(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_C_LINK_EXECUTABLE}")

set(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_C_LINK_EXECUTABLE}")
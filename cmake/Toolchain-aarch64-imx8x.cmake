# cmake/Toolchain-aarch64-imx8x.cmake
# norxs cross-compilation toolchain for NXP i.MX8X (Cortex-A35 / aarch64)
# Targeting NXP Linux BSP (Yocto kirkstone or later) with POSIX RT extensions

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# NXP i.MX8X Cortex-A35 — ARMv8-A, no Thumb, hardware FP
set(NORXS_TARGET_ARCH_FLAGS
    -mcpu=cortex-a35
    -mtune=cortex-a35
    -march=armv8-a+crc
)

# Toolchain prefix — adjust for your SDK installation
set(CROSS_COMPILE "aarch64-poky-linux-")
set(CMAKE_C_COMPILER   "${CROSS_COMPILE}gcc")
set(CMAKE_CXX_COMPILER "${CROSS_COMPILE}g++")
set(CMAKE_AR           "${CROSS_COMPILE}ar"   CACHE FILEPATH "")
set(CMAKE_STRIP        "${CROSS_COMPILE}strip" CACHE FILEPATH "")

# Sysroot — set via NXP SDK environment or NORXS_SYSROOT env var
# Sysroot is only needed for NXP Yocto SDK builds. For generic CI cross-
# compilation with apt's g++-aarch64-linux-gnu, do NOT set CMAKE_SYSROOT —
# the cross-compiler already knows its library paths, and an incorrect
# sysroot will break linking (the C++ standard library lives outside it).
if(DEFINED ENV{NORXS_SYSROOT} AND NOT "$ENV{NORXS_SYSROOT}" STREQUAL "")
    set(CMAKE_SYSROOT $ENV{NORXS_SYSROOT})
elseif(EXISTS "/opt/fsl-imx-wayland/6.1-mickledore/sysroots/armv8a-poky-linux")
    set(CMAKE_SYSROOT "/opt/fsl-imx-wayland/6.1-mickledore/sysroots/armv8a-poky-linux")
# else: no sysroot — cross-compiler uses its default search paths
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Add arch flags to compiler
add_compile_options(${NORXS_TARGET_ARCH_FLAGS})
add_link_options(${NORXS_TARGET_ARCH_FLAGS})

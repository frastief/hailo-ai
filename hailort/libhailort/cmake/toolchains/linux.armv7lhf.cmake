set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_STRIP arm-linux-gnueabihf--strip CACHE FILEPATH "Strip")
set(CMAKE_LINKER arm-linux-gnueabihf-ld)

add_compile_options(-march=armv7-a)

# pybind is not supported in this platform
set(HAILO_BUILD_PYBIND "OFF" CACHE STRING "hailo_build_pybind" FORCE)

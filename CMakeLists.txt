cmake_minimum_required(VERSION 3.20)
project(loony_shim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Werror)
set(CMAKE_C_FLAGS_DEBUG "-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")
set(CMAKE_C_FLAGS_RELEASE "-O2")

find_package(PkgConfig REQUIRED)
pkg_check_modules(UNICORN REQUIRED IMPORTED_TARGET unicorn)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)

file(GLOB CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.c)
list(REMOVE_ITEM CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/main.c)

add_library(loony_core STATIC ${CORE_SOURCES})
target_include_directories(loony_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(loony_core PUBLIC PkgConfig::UNICORN PkgConfig::SDL3 "-framework CoreFoundation"
                      "-framework CoreGraphics" "-framework ImageIO")

add_executable(loony src/main.c)
target_link_libraries(loony PRIVATE loony_core)

# The double-clickable app: cmake --build build-release --target app
add_custom_target(app
  COMMAND ${CMAKE_SOURCE_DIR}/tools/make_app.sh $<TARGET_FILE:loony> ${CMAKE_BINARY_DIR}
  DEPENDS loony
  COMMENT "Building LittleWing.app"
  VERBATIM)

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.c)
add_executable(loony_tests ${TEST_SOURCES})
target_include_directories(loony_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(loony_tests PRIVATE loony_core)
target_compile_definitions(loony_tests PRIVATE LOONY_BIN="$<TARGET_FILE:loony>"
                                               LOONY_SRC_DIR="${CMAKE_SOURCE_DIR}")
add_dependencies(loony_tests loony)

enable_testing()
add_test(NAME unit COMMAND loony_tests)

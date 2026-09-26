cmake_minimum_required(VERSION 3.24)
project(machlin_ext4 VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
option(EXT4_SANITIZERS "Instrument userspace code with ASan and UBSan" ON)

set(EXT4_CORE_SOURCES
    core/support.c core/mount.c core/inode.c core/read.c core/directory.c)
add_library(ext4_core STATIC ${EXT4_CORE_SOURCES})
target_include_directories(ext4_core PUBLIC include)
target_compile_options(ext4_core PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wdeclaration-after-statement
    -ffreestanding -fno-builtin)

# ASan changes frame layout substantially. Enforce the kernel-oriented stack
# budget on a separate optimized freestanding compilation of the identical code.
add_library(ext4_freestanding_check OBJECT ${EXT4_CORE_SOURCES})
target_include_directories(ext4_freestanding_check PRIVATE include)
target_compile_options(ext4_freestanding_check PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wdeclaration-after-statement
    -O2 -ffreestanding -fno-builtin -Wframe-larger-than=2048)

add_library(ext4_posix STATIC adapters/posix/image.c)
target_include_directories(ext4_posix PUBLIC adapters/posix)
target_link_libraries(ext4_posix PUBLIC ext4_core)
target_compile_definitions(ext4_posix PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(ext4_posix PRIVATE -Wall -Wextra -Werror -Wdeclaration-after-statement)

if(EXT4_SANITIZERS)
    foreach(target ext4_core ext4_posix)
        target_compile_options(${target} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PUBLIC -fsanitize=address,undefined)
    endforeach()
endif()

enable_testing()

add_executable(ext4-reader-test tests/reader.c)
target_link_libraries(ext4-reader-test PRIVATE ext4_posix)
target_compile_options(ext4-reader-test PRIVATE -Wall -Wextra -Werror -Wdeclaration-after-statement)
set(EXT4_FIXTURES "${CMAKE_SOURCE_DIR}/artifacts/fixtures" CACHE PATH "Generated fixture directory")
add_test(NAME image-reader COMMAND ext4-reader-test
    "${EXT4_FIXTURES}/ext4-4k.img"
    "${EXT4_FIXTURES}/ext4-1k.img"
    "${EXT4_FIXTURES}/ext4-indexed.img")

add_executable(ext4-corruption-test tests/corruption.c)
target_include_directories(ext4-corruption-test PRIVATE core)
target_link_libraries(ext4-corruption-test PRIVATE ext4_posix)
target_compile_options(ext4-corruption-test PRIVATE -Wall -Wextra -Werror -Wdeclaration-after-statement)
add_test(NAME malformed-images COMMAND ext4-corruption-test "${EXT4_FIXTURES}/ext4-4k.img")

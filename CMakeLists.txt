cmake_minimum_required(VERSION 3.24)
project(machlin_ext4 VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Werror=conditional-uninitialized)
endif()
option(EXT4_SANITIZERS "Instrument userspace code with ASan and UBSan" ON)
option(EXT4_BUILD_FUZZER "Build the bounded metadata mutation fuzzer with Clang" OFF)
option(EXT4_EXTENDED_TESTS "Require the extended generated image profile" OFF)
option(EXT4_INODE128_TEST "Require the generated 128-byte-inode write fixture" OFF)
option(EXT4_UNWRITTEN_TESTS "Require independent unwritten-allocation fixtures" OFF)

set(EXT4_CORE_SOURCES
    core/support.c core/mount.c core/inode.c core/read.c core/directory.c
    core/journal.c core/recovery.c core/group.c core/system.c
    core/allocate.c core/map_write.c core/write.c)
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
if(APPLE)
    target_compile_definitions(ext4_posix PRIVATE _DARWIN_C_SOURCE)
endif()
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
set(EXT4_READER_IMAGES ext4-4k.img ext4-1k.img ext4-indexed.img)
if(EXT4_EXTENDED_TESTS)
    list(APPEND EXT4_READER_IMAGES
        ext4-2k.img ext4-8k.img ext4-16k.img ext4-32k.img ext4-64k.img
        ext4-indirect-1k.img ext4-no-checksum.img ext4-checksum-seed.img)
endif()
list(TRANSFORM EXT4_READER_IMAGES PREPEND "${EXT4_FIXTURES}/")
add_test(NAME image-reader COMMAND ext4-reader-test ${EXT4_READER_IMAGES})

add_executable(ext4-corruption-test tests/corruption.c)
target_include_directories(ext4-corruption-test PRIVATE core)
target_link_libraries(ext4-corruption-test PRIVATE ext4_posix)
target_compile_options(ext4-corruption-test PRIVATE -Wall -Wextra -Werror -Wdeclaration-after-statement)
add_test(NAME malformed-images COMMAND ext4-corruption-test "${EXT4_FIXTURES}/ext4-4k.img")

add_executable(ext4-recover tools/recover.c)
target_link_libraries(ext4-recover PRIVATE ext4_posix)
target_compile_options(ext4-recover PRIVATE -Wall -Wextra -Werror -Wdeclaration-after-statement)

add_executable(ext4-journal-test tests/journal.c)
target_include_directories(ext4-journal-test PRIVATE core)
target_link_libraries(ext4-journal-test PRIVATE ext4_posix)
target_compile_options(ext4-journal-test PRIVATE -Wall -Wextra -Werror -Wdeclaration-after-statement)
add_test(NAME journal-durability-4k COMMAND ext4-journal-test "${EXT4_FIXTURES}/ext4-4k.img")
add_test(NAME journal-durability-1k COMMAND ext4-journal-test "${EXT4_FIXTURES}/ext4-1k.img")
set_tests_properties(journal-durability-4k journal-durability-1k PROPERTIES TIMEOUT 300)

add_executable(ext4-write-test tests/write.c)
target_include_directories(ext4-write-test PRIVATE core)
target_link_libraries(ext4-write-test PRIVATE ext4_posix)
target_compile_options(ext4-write-test PRIVATE -Wall -Wextra -Werror -Wdeclaration-after-statement)
set(EXT4_WRITE_IMAGES "${EXT4_FIXTURES}/ext4-4k.img" "${EXT4_FIXTURES}/ext4-1k.img")
if(EXT4_EXTENDED_TESTS)
    list(APPEND EXT4_WRITE_IMAGES
        "${EXT4_FIXTURES}/ext4-2k.img" "${EXT4_FIXTURES}/ext4-8k.img"
        "${EXT4_FIXTURES}/ext4-16k.img" "${EXT4_FIXTURES}/ext4-32k.img"
        "${EXT4_FIXTURES}/ext4-indirect-1k.img" "${EXT4_FIXTURES}/ext4-no-checksum.img"
        "${EXT4_FIXTURES}/ext4-checksum-seed.img")
endif()
if(EXT4_INODE128_TEST)
    list(APPEND EXT4_WRITE_IMAGES "${EXT4_FIXTURES}/ext4-inode128.img")
endif()
add_test(NAME inode-and-file-write COMMAND ext4-write-test ${EXT4_WRITE_IMAGES})
set_tests_properties(inode-and-file-write PROPERTIES TIMEOUT 300)
add_test(NAME allocation-and-growth COMMAND ext4-write-test --allocation ${EXT4_WRITE_IMAGES})
set_tests_properties(allocation-and-growth PROPERTIES TIMEOUT 600)
add_test(NAME truncate-and-free COMMAND ext4-write-test --truncate ${EXT4_WRITE_IMAGES})
set_tests_properties(truncate-and-free PROPERTIES TIMEOUT 600)
set(EXT4_ALLOCATION_FIXTURES "${CMAKE_SOURCE_DIR}/artifacts/allocation-fixtures" CACHE PATH
    "Independent unwritten-allocation images")
if(EXT4_UNWRITTEN_TESTS)
    add_test(NAME unwritten-allocation COMMAND ext4-write-test --allocation
        "${EXT4_ALLOCATION_FIXTURES}/unwritten-ext4-1k.img"
        "${EXT4_ALLOCATION_FIXTURES}/unwritten-ext4-4k.img"
        "${EXT4_ALLOCATION_FIXTURES}/unwritten-ext4-no-checksum.img"
        "${EXT4_ALLOCATION_FIXTURES}/unwritten-ext4-checksum-seed.img")
    set_tests_properties(unwritten-allocation PROPERTIES TIMEOUT 300)
    add_test(NAME unwritten-truncate COMMAND ext4-write-test --truncate
        "${EXT4_ALLOCATION_FIXTURES}/unwritten-ext4-1k.img"
        "${EXT4_ALLOCATION_FIXTURES}/unwritten-ext4-4k.img"
        "${EXT4_ALLOCATION_FIXTURES}/unwritten-ext4-no-checksum.img"
        "${EXT4_ALLOCATION_FIXTURES}/unwritten-ext4-checksum-seed.img")
    set_tests_properties(unwritten-truncate PROPERTIES TIMEOUT 300)
endif()

find_package(Threads REQUIRED)
add_executable(ext4-mounted-test tests/mounted.c)
target_link_libraries(ext4-mounted-test PRIVATE Threads::Threads)
target_compile_options(ext4-mounted-test PRIVATE -Wall -Wextra -Werror -Wdeclaration-after-statement)

if(APPLE)
    add_executable(ext4-mounted-lifetime-test tests/mounted_lifetime.c)
    target_compile_options(ext4-mounted-lifetime-test PRIVATE
        -Wall -Wextra -Werror -Wdeclaration-after-statement)
endif()

if(EXT4_BUILD_FUZZER)
    add_executable(ext4-image-fuzzer tests/fuzz_image.c ${EXT4_CORE_SOURCES})
    target_include_directories(ext4-image-fuzzer PRIVATE core include)
    target_compile_options(ext4-image-fuzzer PRIVATE
        -Wall -Wextra -Werror -Wdeclaration-after-statement
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(ext4-image-fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

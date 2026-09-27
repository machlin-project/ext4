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
option(EXT4_ORPHAN_FILE_TESTS "Require independently allocated orphan-file fixtures" OFF)
option(EXT4_NAMESPACE_TESTS "Require small multi-group namespace fixtures" OFF)

set(EXT4_CORE_SOURCES
    core/support.c core/mount.c core/inode.c core/read.c core/directory.c
    core/journal.c core/recovery.c core/group.c core/system.c
    core/allocate.c core/map_write.c core/write.c core/orphan.c
    core/inode_allocate.c core/namespace.c core/lifetime.c core/directory_hash.c
    core/directory_index.c core/directory_write.c)
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

add_executable(ext4-space-test tests/space.c)
target_include_directories(ext4-space-test PRIVATE core)
target_link_libraries(ext4-space-test PRIVATE ext4_posix)
target_compile_options(ext4-space-test PRIVATE -Wall -Wextra -Werror -Wdeclaration-after-statement)
set(EXT4_SPACE_FIXTURES "" CACHE PATH "Independent fully allocated namespace images")
if(EXT4_SPACE_FIXTURES)
    foreach(profile 1k 4k indirect no-checksum no-filetype inode128 checksum-seed orphan-file)
        add_test(NAME "namespace-space-${profile}" COMMAND ext4-space-test
            "${EXT4_SPACE_FIXTURES}/space-${profile}.img")
        set_tests_properties("namespace-space-${profile}" PROPERTIES TIMEOUT 300)
    endforeach()
endif()

add_executable(ext4-directory-hash-test tests/directory_hash.c)
target_include_directories(ext4-directory-hash-test PRIVATE core)
target_link_libraries(ext4-directory-hash-test PRIVATE ext4_core)
target_compile_options(ext4-directory-hash-test PRIVATE -Wall -Wextra -Werror -Wdeclaration-after-statement)
add_test(NAME directory-hash COMMAND ext4-directory-hash-test)

add_executable(ext4-directory-index-test tests/directory_index.c)
target_include_directories(ext4-directory-index-test PRIVATE core)
target_link_libraries(ext4-directory-index-test PRIVATE ext4_posix)
target_compile_options(ext4-directory-index-test PRIVATE -Wall -Wextra -Werror -Wdeclaration-after-statement)
add_executable(ext4-index-write-test tests/index_write.c)
target_include_directories(ext4-index-write-test PRIVATE core)
target_link_libraries(ext4-index-write-test PRIVATE ext4_posix)
target_compile_options(ext4-index-write-test PRIVATE -Wall -Wextra -Werror -Wdeclaration-after-statement)
set(EXT4_INDEX_FIXTURES "" CACHE PATH "Independent indexed-directory images")
set(EXT4_INDEX_COLLISIONS "" CACHE FILEPATH "Independently verified directory hash collisions")
set(EXT4_INDEX_CAPACITY "" CACHE FILEPATH "Independent full-root indexed directory image")
if(EXT4_INDEX_CAPACITY)
    add_test(NAME indexed-capacity COMMAND ext4-index-write-test --capacity "${EXT4_INDEX_CAPACITY}")
    set_tests_properties(indexed-capacity PROPERTIES TIMEOUT 300)
endif()
if(EXT4_INDEX_FIXTURES)
    foreach(profile legacy-signed legacy-unsigned half_md4-signed half_md4-unsigned
            tea-signed tea-unsigned 2k 4k 8k 16k 32k deep deep-unsigned indirect
            no-checksum no-filetype inode128 checksum-seed orphan-file orphan-file-deep)
        add_test(NAME "directory-index-${profile}" COMMAND ext4-directory-index-test
            "${EXT4_INDEX_FIXTURES}/index-${profile}.img")
        set_tests_properties("directory-index-${profile}" PROPERTIES TIMEOUT 300)
        add_test(NAME "indexed-write-${profile}" COMMAND ext4-index-write-test
            "${EXT4_INDEX_FIXTURES}/index-${profile}.img")
        set_tests_properties("indexed-write-${profile}" PROPERTIES TIMEOUT 300)
        add_test(NAME "indexed-faults-${profile}" COMMAND ext4-index-write-test --faults
            "${EXT4_INDEX_FIXTURES}/index-${profile}.img")
        set_tests_properties("indexed-faults-${profile}" PROPERTIES TIMEOUT 900)
        if(EXT4_INDEX_COLLISIONS AND NOT profile MATCHES "^(2k|4k|8k|16k|32k)$")
            add_test(NAME "indexed-edges-${profile}" COMMAND ext4-index-write-test
                --edges "${EXT4_INDEX_COLLISIONS}" "${EXT4_INDEX_FIXTURES}/index-${profile}.img")
            set_tests_properties("indexed-edges-${profile}" PROPERTIES TIMEOUT 300)
        endif()
    endforeach()
endif()

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
add_executable(ext4-namespace-test tests/namespace.c)
target_include_directories(ext4-namespace-test PRIVATE core)
target_link_libraries(ext4-namespace-test PRIVATE ext4_posix)
target_compile_options(ext4-namespace-test PRIVATE -Wall -Wextra -Werror -Wdeclaration-after-statement)
add_test(NAME namespace-create-and-link COMMAND ext4-namespace-test ${EXT4_WRITE_IMAGES})
add_test(NAME namespace-symlinks COMMAND ext4-namespace-test --symlinks ${EXT4_WRITE_IMAGES})
set_tests_properties(namespace-create-and-link PROPERTIES TIMEOUT 900)
set_tests_properties(namespace-symlinks PROPERTIES TIMEOUT 900)
add_executable(ext4-removal-test tests/removal.c)
target_include_directories(ext4-removal-test PRIVATE core)
target_link_libraries(ext4-removal-test PRIVATE ext4_posix)
target_compile_options(ext4-removal-test PRIVATE -Wall -Wextra -Werror -Wdeclaration-after-statement)
function(ext4_add_removal_tests)
    foreach(image IN LISTS ARGN)
        get_filename_component(profile "${image}" NAME_WE)
        add_test(NAME "namespace-removal-${profile}" COMMAND ext4-removal-test "${image}")
        set_tests_properties("namespace-removal-${profile}" PROPERTIES TIMEOUT 900)
    endforeach()
endfunction()
ext4_add_removal_tests(${EXT4_WRITE_IMAGES})
add_test(NAME namespace-removal-indexed-validation COMMAND ext4-removal-test --indexed
    "${EXT4_FIXTURES}/ext4-indexed.img")
add_executable(ext4-rename-test tests/rename.c)
target_include_directories(ext4-rename-test PRIVATE core)
target_link_libraries(ext4-rename-test PRIVATE ext4_posix)
target_compile_options(ext4-rename-test PRIVATE -Wall -Wextra -Werror -Wdeclaration-after-statement)
# Each image owns its full functional/fault matrix. A timeout must not discard
# later profiles in a large aggregate invocation on a slower CI runner.
function(ext4_add_rename_tests)
    foreach(image IN LISTS ARGN)
        get_filename_component(profile "${image}" NAME_WE)
        add_test(NAME "namespace-rename-${profile}" COMMAND ext4-rename-test "${image}")
        set_tests_properties("namespace-rename-${profile}" PROPERTIES TIMEOUT 900)
    endforeach()
endfunction()
ext4_add_rename_tests(${EXT4_WRITE_IMAGES})
add_test(NAME namespace-rename-indexed COMMAND ext4-rename-test --indexed
    "${EXT4_FIXTURES}/ext4-indexed.img")
add_test(NAME namespace-indexed COMMAND ext4-namespace-test --indexed
    "${EXT4_FIXTURES}/ext4-indexed.img")
set(EXT4_NAMESPACE_FIXTURES "${CMAKE_SOURCE_DIR}/artifacts/namespace-fixtures" CACHE PATH
    "Independent namespace allocation images")
if(EXT4_NAMESPACE_TESTS)
    set(EXT4_NAMESPACE_IMAGES)
    foreach(profile small lazy indirect no-checksum no-filetype inode128)
        list(APPEND EXT4_NAMESPACE_IMAGES "${EXT4_NAMESPACE_FIXTURES}/namespace-${profile}.img")
    endforeach()
    add_test(NAME namespace-small-formats COMMAND ext4-namespace-test ${EXT4_NAMESPACE_IMAGES})
    add_test(NAME namespace-inode-exhaustion COMMAND ext4-namespace-test --exhaust ${EXT4_NAMESPACE_IMAGES})
    add_test(NAME namespace-inode-group-transition COMMAND ext4-namespace-test --groups ${EXT4_NAMESPACE_IMAGES})
    add_test(NAME namespace-symlink-small-formats COMMAND ext4-namespace-test --symlinks ${EXT4_NAMESPACE_IMAGES})
    add_test(NAME namespace-symlink-inode-groups COMMAND ext4-namespace-test --symlinks --groups ${EXT4_NAMESPACE_IMAGES})
    ext4_add_removal_tests(${EXT4_NAMESPACE_IMAGES})
    ext4_add_rename_tests(${EXT4_NAMESPACE_IMAGES})
    set_tests_properties(namespace-small-formats namespace-inode-exhaustion
        namespace-inode-group-transition namespace-symlink-small-formats
        namespace-symlink-inode-groups PROPERTIES TIMEOUT 600)
endif()
add_executable(ext4-orphan-test tests/orphan.c)
target_include_directories(ext4-orphan-test PRIVATE core)
target_link_libraries(ext4-orphan-test PRIVATE ext4_posix)
target_compile_options(ext4-orphan-test PRIVATE -Wall -Wextra -Werror -Wdeclaration-after-statement)
add_test(NAME orphan-truncate-recovery COMMAND ext4-orphan-test ${EXT4_WRITE_IMAGES})
set_tests_properties(orphan-truncate-recovery PROPERTIES TIMEOUT 600)
add_test(NAME live-truncate COMMAND ext4-orphan-test --live ${EXT4_WRITE_IMAGES})
set_tests_properties(live-truncate PROPERTIES TIMEOUT 900)
set(EXT4_ORPHAN_FILE_FIXTURES "${CMAKE_SOURCE_DIR}/artifacts/orphan-file-fixtures" CACHE PATH
    "Independent orphan-file images")
if(EXT4_ORPHAN_FILE_TESTS)
    set(EXT4_ORPHAN_FILE_IMAGES)
    foreach(source IN LISTS EXT4_WRITE_IMAGES)
        get_filename_component(name "${source}" NAME)
        list(APPEND EXT4_ORPHAN_FILE_IMAGES "${EXT4_ORPHAN_FILE_FIXTURES}/orphan-file-${name}")
    endforeach()
    add_test(NAME orphan-file-recovery COMMAND ext4-orphan-test --orphan-file ${EXT4_ORPHAN_FILE_IMAGES})
    add_test(NAME orphan-file-mixed-recovery COMMAND ext4-orphan-test --mixed
        "${EXT4_ORPHAN_FILE_FIXTURES}/orphan-file-ext4-1k.img"
        "${EXT4_ORPHAN_FILE_FIXTURES}/orphan-file-ext4-4k.img")
    add_test(NAME orphan-file-live-truncate COMMAND ext4-orphan-test --live ${EXT4_ORPHAN_FILE_IMAGES})
    add_test(NAME namespace-orphan-file COMMAND ext4-namespace-test ${EXT4_ORPHAN_FILE_IMAGES})
    add_test(NAME namespace-symlink-orphan-file COMMAND ext4-namespace-test --symlinks ${EXT4_ORPHAN_FILE_IMAGES})
    set_tests_properties(namespace-orphan-file PROPERTIES TIMEOUT 900)
    set_tests_properties(namespace-symlink-orphan-file PROPERTIES TIMEOUT 900)
    ext4_add_removal_tests(${EXT4_ORPHAN_FILE_IMAGES})
    ext4_add_rename_tests(${EXT4_ORPHAN_FILE_IMAGES})
    set_tests_properties(orphan-file-recovery orphan-file-mixed-recovery orphan-file-live-truncate
        PROPERTIES TIMEOUT 900)
    if(EXT4_EXTENDED_TESTS)
        add_test(NAME orphan-file-indirect-recovery COMMAND ext4-orphan-test --orphan-file
            "${EXT4_ORPHAN_FILE_FIXTURES}/mapped/orphan-file-ext4-indirect-1k.img")
        add_test(NAME orphan-file-size-bound COMMAND ext4-orphan-test --orphan-file --smoke
            "${EXT4_ORPHAN_FILE_FIXTURES}/maximum/orphan-file-ext4-indirect-1k.img")
        set_tests_properties(orphan-file-indirect-recovery orphan-file-size-bound PROPERTIES TIMEOUT 900)
    endif()
endif()
if(EXT4_EXTENDED_TESTS)
    add_test(NAME live-truncate-large COMMAND ext4-orphan-test --live --large
        "${EXT4_FIXTURES}/ext4-indirect-1k.img")
    set_tests_properties(live-truncate-large PROPERTIES TIMEOUT 900)
endif()
set(EXT4_ALLOCATION_FIXTURES "${CMAKE_SOURCE_DIR}/artifacts/allocation-fixtures" CACHE PATH
    "Independent unwritten-allocation images")
if(EXT4_UNWRITTEN_TESTS)
    add_test(NAME live-truncate-unwritten COMMAND ext4-orphan-test --live
        "${EXT4_ALLOCATION_FIXTURES}/unwritten-ext4-1k.img"
        "${EXT4_ALLOCATION_FIXTURES}/unwritten-ext4-4k.img"
        "${EXT4_ALLOCATION_FIXTURES}/unwritten-ext4-no-checksum.img"
        "${EXT4_ALLOCATION_FIXTURES}/unwritten-ext4-checksum-seed.img")
    set_tests_properties(live-truncate-unwritten PROPERTIES TIMEOUT 600)
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
add_executable(ext4-mounted-symlink-test tests/mounted_symlinks.c)
target_link_libraries(ext4-mounted-symlink-test PRIVATE Threads::Threads)
target_compile_options(ext4-mounted-symlink-test PRIVATE -Wall -Wextra -Werror -Wdeclaration-after-statement)

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

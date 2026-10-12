cmake_minimum_required(VERSION 3.20)
project(Aps5Translator C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(BUILD_TESTING "Build and register translator tests" ON)
option(APS5_TRANSLATOR_TSO "Order guest loads and stores as x86 does" ON)
option(APS5_TRANSLATOR_VECTOR_TSO "Also order guest vector loads and stores" ON)

set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_ENCODER OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/zydis ${CMAKE_CURRENT_BINARY_DIR}/zydis EXCLUDE_FROM_ALL)

add_library(translator_elf STATIC src/ElfImage.cpp)
target_include_directories(translator_elf PUBLIC src)

add_executable(x64a64 src/main.cpp src/Discovery.cpp src/Emitter.cpp)
target_link_libraries(x64a64 PRIVATE translator_elf Zydis)

add_library(translator_runtime STATIC runtime/src/Runtime.cpp runtime/src/HostCall.S)
target_include_directories(translator_runtime PUBLIC runtime/include)
target_link_libraries(translator_runtime PUBLIC translator_elf)
target_compile_definitions(translator_runtime PUBLIC APS5_TRANSLATOR_TSO=$<BOOL:${APS5_TRANSLATOR_TSO}>
    APS5_TRANSLATOR_VECTOR_TSO=$<BOOL:${APS5_TRANSLATOR_VECTOR_TSO}>)

if(BUILD_TESTING)
    enable_testing()
    set(guestLinkFlags --target=x86_64-unknown-linux-gnu -fuse-ld=lld -nostdlib)
    set(guestFlags --target=x86_64-unknown-linux-gnu -O2 -fPIC -ffreestanding -fno-stack-protector
        -fno-asynchronous-unwind-tables -fno-math-errno -ffp-contract=off -I${CMAKE_CURRENT_SOURCE_DIR}/tests/programs)
    set(stubLibrary ${CMAKE_CURRENT_BINARY_DIR}/guest/libhost.so)
    add_custom_command(OUTPUT ${stubLibrary}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/guest
        COMMAND ${CMAKE_C_COMPILER} ${guestFlags} -c ${CMAKE_CURRENT_SOURCE_DIR}/tests/host/HostStub.c -o ${CMAKE_CURRENT_BINARY_DIR}/guest/HostStub.o
        COMMAND ${CMAKE_C_COMPILER} ${guestLinkFlags} -shared -Wl,-soname,libhost.so ${CMAKE_CURRENT_BINARY_DIR}/guest/HostStub.o -o ${stubLibrary}
        DEPENDS tests/host/HostStub.c
        VERBATIM)

    function(add_translation_test name source)
        set(variant ${ARGN})
        set(guestDir ${CMAKE_CURRENT_BINARY_DIR}/guest)
        set(elf ${guestDir}/${name}.elf)
        set(generated ${CMAKE_CURRENT_BINARY_DIR}/generated/${name}.cpp)
        add_custom_command(OUTPUT ${elf}
            COMMAND ${CMAKE_C_COMPILER} ${guestFlags} ${variant} -c ${CMAKE_CURRENT_SOURCE_DIR}/tests/programs/${source} -o ${guestDir}/${name}.o
            COMMAND ${CMAKE_C_COMPILER} ${guestLinkFlags} -pie -Wl,-z,norelro -Wl,-e,test_main ${guestDir}/${name}.o ${stubLibrary} -o ${elf}
            DEPENDS tests/programs/${source} tests/programs/Host.h ${stubLibrary}
            VERBATIM)
        add_custom_command(OUTPUT ${generated} ${CMAKE_CURRENT_BINARY_DIR}/generated/${name}.report.txt
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
            COMMAND x64a64 ${elf} -o ${generated} --report > ${CMAKE_CURRENT_BINARY_DIR}/generated/${name}.report.txt
            DEPENDS x64a64 ${elf}
            VERBATIM)
        add_executable(${name}_translated ${generated} tests/host/TranslatedHost.cpp)
        target_link_libraries(${name}_translated PRIVATE translator_runtime)
        target_compile_options(${name}_translated PRIVATE -Wno-unused-label -Wno-unused-variable -Wno-unused-value)
        add_executable(${name}_native tests/programs/${source} tests/host/NativeHost.c)
        target_compile_options(${name}_native PRIVATE -O2 -ffp-contract=off)
        add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
            -DNATIVE=$<TARGET_FILE:${name}_native> -DTRANSLATED=$<TARGET_FILE:${name}_translated> -DELF=${elf}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/Compare.cmake)
        set_tests_properties(${name} PROPERTIES TIMEOUT 120)
    endfunction()

    foreach(program Integer Float Callbacks Threads Bench)
        add_translation_test(${program} ${program}.c)
        add_translation_test(${program}Zen2 ${program}.c -march=znver2)
    endforeach()
endif()

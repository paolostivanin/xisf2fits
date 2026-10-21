cmake_minimum_required(VERSION 3.16)
project(xisf2fits VERSION 0.5.0 LANGUAGES CXX)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CFITSIO REQUIRED IMPORTED_TARGET cfitsio)
pkg_check_modules(XML REQUIRED IMPORTED_TARGET libxml-2.0)
pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(ZLIB 1.2.9 REQUIRED)
find_package(Threads REQUIRED)

add_library(converter STATIC src/common.cpp src/source.cpp src/metadata.cpp src/fits.cpp)
target_include_directories(converter PUBLIC src)
target_link_libraries(converter PUBLIC PkgConfig::CFITSIO PkgConfig::XML PkgConfig::LZ4
    PkgConfig::ZSTD OpenSSL::Crypto ZLIB::ZLIB Threads::Threads)
add_executable(xisf2fits src/main.cpp)
target_link_libraries(xisf2fits PRIVATE converter)
target_compile_definitions(xisf2fits PRIVATE XISF2FITS_VERSION="${PROJECT_VERSION}")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  foreach(target converter xisf2fits)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wformat=2 -Werror=format-security
        -fstack-protector-strong
        -include ${CMAKE_CURRENT_SOURCE_DIR}/src/hardening.hpp
    )
  endforeach()
endif()

option(ENABLE_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
if(ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(converter PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(converter PUBLIC -fsanitize=address,undefined)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(converter xisf2fits PROPERTIES POSITION_INDEPENDENT_CODE ON)
    include(CheckPIESupported)
    check_pie_supported(LANGUAGES CXX)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(xisf2fits PRIVATE -Wl,-z,relro -Wl,-z,now)
    endif()
endif()

install(TARGETS xisf2fits RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    add_executable(unit_tests tests/unit.cpp)
    target_link_libraries(unit_tests PRIVATE converter)
    add_test(NAME unit COMMAND unit_tests)
    find_package(Python3 COMPONENTS Interpreter)
    set(TEST_DEPS 1)
    if(Python3_Interpreter_FOUND)
        execute_process(COMMAND ${Python3_EXECUTABLE} -c "import astropy, numpy, lz4.block, zstandard"
            RESULT_VARIABLE TEST_DEPS OUTPUT_QUIET ERROR_QUIET)
    endif()
    if(NOT TEST_DEPS EQUAL 0)
        message(WARNING "Integration tests disabled: they need Python packages from tests/requirements.txt. Use a venv and -DPython3_EXECUTABLE=..., or -DBUILD_TESTING=OFF.")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_library(test_faults SHARED tests/faults.cpp)
        target_link_libraries(test_faults PRIVATE ${CMAKE_DL_LIBS})
        add_test(NAME integration COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py
            $<TARGET_FILE:xisf2fits> $<TARGET_FILE:test_faults>)
        if(ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libasan.so OUTPUT_VARIABLE ASAN_RUNTIME OUTPUT_STRIP_TRAILING_WHITESPACE)
            set_tests_properties(integration PROPERTIES ENVIRONMENT "XISF2FITS_TEST_ASAN_RUNTIME=${ASAN_RUNTIME}")
        endif()
    else()
        add_test(NAME integration COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py $<TARGET_FILE:xisf2fits>)
    endif()
    if(TEST integration)
        set_tests_properties(integration PROPERTIES TIMEOUT 180)
    endif()
endif()

cmake_minimum_required(VERSION 3.16)
project(atree VERSION 0.1.0 LANGUAGES C)

option(ATREE_BUILD_SHARED "Build the shared library" ON)
option(ATREE_BUILD_TESTS "Build tests" ON)
option(ATREE_BUILD_BENCH "Build benchmarks" OFF)
option(ATREE_WERROR "Treat warnings as errors" ON)
set(ATREE_SANITIZE "" CACHE STRING "Sanitizers, e.g. address,undefined or thread")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

file(GLOB ATREE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

if(MSVC)
    set(ATREE_WARNINGS /W4)
    if(ATREE_WERROR)
        list(APPEND ATREE_WARNINGS /WX)
    endif()
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    set(ATREE_WARNINGS
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wcast-align
        -Wpointer-arith -Wwrite-strings -Wformat=2 -Wundef -Wvla
        -Wswitch-enum -Wswitch-default -Wfloat-equal -Wdouble-promotion)
    if(ATREE_WERROR)
        list(APPEND ATREE_WARNINGS -Werror)
    endif()
endif()

if(ATREE_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=${ATREE_SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${ATREE_SANITIZE})
endif()

# ---- library -----------------------------------------------------------------

add_library(atree_static STATIC ${ATREE_SOURCES})
set_target_properties(atree_static PROPERTIES OUTPUT_NAME atree)
target_include_directories(atree_static
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(atree_static PRIVATE ${ATREE_WARNINGS})
target_compile_definitions(atree_static PRIVATE ATREE_BUILDING)
add_library(atree::atree_static ALIAS atree_static)

set(ATREE_INSTALL_TARGETS atree_static)

if(ATREE_BUILD_SHARED)
    add_library(atree_shared SHARED ${ATREE_SOURCES})
    set_target_properties(atree_shared PROPERTIES
        OUTPUT_NAME atree
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})
    target_include_directories(atree_shared
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(atree_shared PRIVATE ${ATREE_WARNINGS})
    target_compile_definitions(atree_shared PRIVATE ATREE_BUILDING ATREE_SHARED
                               INTERFACE ATREE_SHARED)
    add_library(atree::atree ALIAS atree_shared)
    list(APPEND ATREE_INSTALL_TARGETS atree_shared)
endif()

# ---- tests -------------------------------------------------------------------

if(ATREE_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)
    file(GLOB ATREE_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.c)
    foreach(src ${ATREE_TEST_SOURCES})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE atree_static Threads::Threads)
        target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_options(${name} PRIVATE ${ATREE_WARNINGS})
        add_test(NAME ${name} COMMAND ${name})
    endforeach()

    add_executable(header_c tests/header_c.c)
    target_link_libraries(header_c PRIVATE atree_static)
    target_compile_options(header_c PRIVATE ${ATREE_WARNINGS})
    add_test(NAME header_c COMMAND header_c)

    include(CheckLanguage)
    check_language(CXX)
    if(CMAKE_CXX_COMPILER)
        enable_language(CXX)
        add_executable(header_cxx tests/header_cxx.cpp)
        target_link_libraries(header_cxx PRIVATE atree_static)
        set_target_properties(header_cxx PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
        add_test(NAME header_cxx COMMAND header_cxx)
    endif()
endif()

# ---- benchmarks --------------------------------------------------------------

if(ATREE_BUILD_BENCH)
    file(GLOB ATREE_BENCH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bench/*.c)
    foreach(src ${ATREE_BENCH_SOURCES})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE atree_static)
        target_compile_options(${name} PRIVATE ${ATREE_WARNINGS})
    endforeach()
endif()

# ---- install -----------------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS ${ATREE_INSTALL_TARGETS}
    EXPORT atreeTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/atree.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT atreeTargets NAMESPACE atree:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/atree)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/atreeConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/atreeConfig.cmake
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/atreeTargets.cmake\")\n")
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/atreeConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/atreeConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/atree)

set(PREFIX ${CMAKE_INSTALL_PREFIX})
set(VERSION ${PROJECT_VERSION})
configure_file(atree.pc.in ${CMAKE_CURRENT_BINARY_DIR}/atree.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/atree.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

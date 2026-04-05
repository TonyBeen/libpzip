cmake_minimum_required(VERSION 3.15)

project(pzip VERSION 1.0.0 LANGUAGES C CXX)

if(MSVC)
    # Force UTF-8 source/execution charset on Windows to avoid codepage warnings.
    add_compile_options(
        $<$<COMPILE_LANGUAGE:C>:/utf-8>
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
    )
endif()

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(PZIP_BUILD_SHARED "Build pzip shared library" ON)
option(PZIP_BUILD_STATIC "Build pzip static library" ON)
option(PZIP_ENABLE_EXAMPLES "Build pzip examples" ON)
option(PZIP_USE_BUNDLED_ZLIB "Use third_party bundled zlib" ON)
option(PZIP_USE_BUNDLED_ZSTD "Use third_party bundled zstd" ON)
option(PZIP_USE_BUNDLED_LZ4 "Use third_party bundled lz4" ON)
option(PZIP_ENABLE_DEBUG "Enable debug mode" OFF)

if(NOT PZIP_BUILD_SHARED AND NOT PZIP_BUILD_STATIC)
    message(FATAL_ERROR "At least one of PZIP_BUILD_SHARED or PZIP_BUILD_STATIC must be ON.")
endif()

set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type (Release MinSizeRel RelWithDebInfo Debug)")

if(PZIP_ENABLE_DEBUG)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type (Release MinSizeRel RelWithDebInfo Debug)" FORCE)
endif()

set(PZIP_BUNDLED_EXPORT_TARGETS)

if(PZIP_USE_BUNDLED_ZLIB)
    set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_SHARED ON CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/zlib EXCLUDE_FROM_ALL)

    set(PZIP_ZLIB_SHARED_TARGET zlib)
    set(PZIP_ZLIB_STATIC_TARGET zlibstatic)
    if(TARGET zlib)
        set(PZIP_ZLIB_SHARED_TARGET zlib)
    elseif(TARGET zlibstatic)
        set(PZIP_ZLIB_SHARED_TARGET zlibstatic)
    endif()
    if(TARGET zlibstatic)
        set(PZIP_ZLIB_STATIC_TARGET zlibstatic)
    elseif(TARGET zlib)
        set(PZIP_ZLIB_STATIC_TARGET zlib)
    endif()

    if(TARGET zlib)
        list(APPEND PZIP_BUNDLED_EXPORT_TARGETS zlib)
    endif()
    if(TARGET zlibstatic)
        list(APPEND PZIP_BUNDLED_EXPORT_TARGETS zlibstatic)
    endif()
else()
    find_package(ZLIB REQUIRED)
    set(PZIP_ZLIB_SHARED_TARGET ZLIB::ZLIB)
    set(PZIP_ZLIB_STATIC_TARGET ZLIB::ZLIB)
endif()

if(PZIP_USE_BUNDLED_ZSTD)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED ON CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    add_subdirectory(third_party/zstd/build/cmake EXCLUDE_FROM_ALL)

    set(PZIP_ZSTD_SHARED_TARGET libzstd_shared)
    set(PZIP_ZSTD_STATIC_TARGET libzstd_static)
    if(NOT TARGET libzstd_shared AND TARGET libzstd)
        set(PZIP_ZSTD_SHARED_TARGET libzstd)
    endif()
    if(NOT TARGET libzstd_static AND TARGET libzstd)
        set(PZIP_ZSTD_STATIC_TARGET libzstd)
    endif()

    if(TARGET libzstd)
        list(APPEND PZIP_BUNDLED_EXPORT_TARGETS libzstd)
    endif()
    if(TARGET libzstd_shared)
        list(APPEND PZIP_BUNDLED_EXPORT_TARGETS libzstd_shared)
    endif()
    if(TARGET libzstd_static)
        list(APPEND PZIP_BUNDLED_EXPORT_TARGETS libzstd_static)
    endif()
endif()

if(PZIP_BUNDLED_EXPORT_TARGETS)
    list(REMOVE_DUPLICATES PZIP_BUNDLED_EXPORT_TARGETS)
endif()

if(PZIP_USE_BUNDLED_LZ4)
    set(PZIP_LZ4_SOURCES
        third_party/lz4/lib/lz4.c
        third_party/lz4/lib/lz4hc.c
    )
    set(PZIP_LZ4_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4/lib)
    set(PZIP_LZ4_LIBRARIES)
else()
    find_path(PZIP_LZ4_INCLUDE_DIR NAMES lz4.h)
    find_library(PZIP_LZ4_LIBRARY NAMES lz4 liblz4)

    if(NOT PZIP_LZ4_INCLUDE_DIR OR NOT PZIP_LZ4_LIBRARY)
        message(FATAL_ERROR
            "System LZ4 not found. Install liblz4 development package or set PZIP_USE_BUNDLED_LZ4=ON.")
    endif()

    set(PZIP_LZ4_SOURCES)
    set(PZIP_LZ4_INCLUDE_DIRS ${PZIP_LZ4_INCLUDE_DIR})
    set(PZIP_LZ4_LIBRARIES ${PZIP_LZ4_LIBRARY})
endif()

find_package(Threads REQUIRED)
set(PZIP_EXTRA_LIBS Threads::Threads)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    list(APPEND PZIP_EXTRA_LIBS stdc++fs)
endif()

set(PZIP_PUBLIC_HEADERS
    include/pzip.h
)

set(PZIP_SOURCES
    src/api/pzip_c_api.cpp
    src/core/pzip_engine.cpp
    ${PZIP_LZ4_SOURCES}
    src/codec/zlib_codec.cpp
    src/codec/lz4_codec.cpp
    src/codec/zstd_codec.cpp
    src/io/file_reader.cpp
    src/platform/time_provider.cpp
    src/zip/zip_writer.cpp
)

add_library(pzip_obj OBJECT ${PZIP_SOURCES})
set_target_properties(pzip_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(pzip_obj PUBLIC cxx_std_11)
target_include_directories(pzip_obj
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/filesystem/include
        ${PZIP_LZ4_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd/lib
        ${CMAKE_CURRENT_BINARY_DIR}/third_party/zlib
        ${CMAKE_CURRENT_BINARY_DIR}/third_party/zstd/build/cmake/lib
)
target_compile_definitions(pzip_obj PRIVATE PZIP_IMPLEMENTATION)

if(MSVC)
    target_compile_options(pzip_obj PRIVATE /W4 /EHsc)
else()
    target_compile_options(pzip_obj PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PZIP_BUILD_SHARED)
    add_library(pzip_shared SHARED $<TARGET_OBJECTS:pzip_obj>)
    target_compile_definitions(pzip_shared PRIVATE PZIP_BUILD_SHARED)
    target_link_libraries(pzip_shared PRIVATE ${PZIP_EXTRA_LIBS} ${PZIP_ZLIB_SHARED_TARGET} ${PZIP_LZ4_LIBRARIES})
    if(PZIP_USE_BUNDLED_ZSTD)
        target_link_libraries(pzip_shared PRIVATE ${PZIP_ZSTD_SHARED_TARGET})
    endif()
    target_include_directories(pzip_shared
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    if(WIN32)
        # Keep DLL name as pzip, but give import lib a unique name on Windows
        # to avoid colliding with the static library artifact.
        set_target_properties(pzip_shared PROPERTIES
            OUTPUT_NAME pzip
            ARCHIVE_OUTPUT_NAME pzip_shared
            VERSION ${PROJECT_VERSION}
            SOVERSION ${PROJECT_VERSION_MAJOR})
    else()
        set_target_properties(pzip_shared PROPERTIES
            OUTPUT_NAME pzip
            VERSION ${PROJECT_VERSION}
            SOVERSION ${PROJECT_VERSION_MAJOR})
    endif()
    install(TARGETS pzip_shared EXPORT pzipTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

if(PZIP_BUILD_STATIC)
    add_library(pzip_static STATIC $<TARGET_OBJECTS:pzip_obj>)
    target_link_libraries(pzip_static PRIVATE ${PZIP_EXTRA_LIBS} ${PZIP_ZLIB_STATIC_TARGET} ${PZIP_LZ4_LIBRARIES})
    if(PZIP_USE_BUNDLED_ZSTD)
        target_link_libraries(pzip_static PRIVATE ${PZIP_ZSTD_STATIC_TARGET})
    endif()
    target_include_directories(pzip_static
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    # Always use libpzip_static.a (or platform equivalent) for static library.
    set_target_properties(pzip_static PROPERTIES OUTPUT_NAME pzip_static)
    install(TARGETS pzip_static EXPORT pzipTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

if(TARGET pzip_shared)
    add_library(pzip::pzip_shared ALIAS pzip_shared)
endif()

if(TARGET pzip_static)
    add_library(pzip::pzip_static ALIAS pzip_static)
endif()

add_library(pzip INTERFACE)
if(TARGET pzip_static)
    target_link_libraries(pzip INTERFACE pzip_static ${PZIP_EXTRA_LIBS} ${PZIP_ZLIB_STATIC_TARGET} ${PZIP_LZ4_LIBRARIES})
    if(PZIP_USE_BUNDLED_ZSTD)
        target_link_libraries(pzip INTERFACE ${PZIP_ZSTD_STATIC_TARGET})
    endif()
elseif(TARGET pzip_shared)
    target_link_libraries(pzip INTERFACE pzip_shared)
endif()

add_library(pzip::pzip ALIAS pzip)

install(FILES ${PZIP_PUBLIC_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(PZIP_BUNDLED_EXPORT_TARGETS)
    install(TARGETS ${PZIP_BUNDLED_EXPORT_TARGETS} EXPORT pzipTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pzipConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/pzipConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pzip
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/pzipConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(EXPORT pzipTargets
    FILE pzipTargets.cmake
    NAMESPACE pzip::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pzip)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/pzipConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/pzipConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pzip)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
    IMMEDIATE
    @ONLY)

if(TARGET uninstall)
    add_custom_target(pzip_uninstall
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
        COMMENT "Uninstalling pzip files from install_manifest.txt")
else()
    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
        COMMENT "Uninstalling pzip files from install_manifest.txt")
endif()

if(PZIP_ENABLE_EXAMPLES)
    add_executable(pzip_example_c examples/minimal_c.c)
    if(PZIP_BUILD_SHARED)
        target_link_libraries(pzip_example_c PRIVATE pzip_shared)
    elseif(PZIP_BUILD_STATIC)
        target_link_libraries(pzip_example_c PRIVATE pzip_static)
    endif()
    target_include_directories(pzip_example_c PRIVATE include)

    add_executable(pzip_example_encrypt_reserved_c examples/encryption_reserved_c.c)
    if(PZIP_BUILD_SHARED)
        target_link_libraries(pzip_example_encrypt_reserved_c PRIVATE pzip_shared)
    elseif(PZIP_BUILD_STATIC)
        target_link_libraries(pzip_example_encrypt_reserved_c PRIVATE pzip_static)
    endif()
    target_include_directories(pzip_example_encrypt_reserved_c PRIVATE include)

    add_executable(pzip_example_zstd_c examples/zstd_codec_c.c)
    if(PZIP_BUILD_SHARED)
        target_link_libraries(pzip_example_zstd_c PRIVATE pzip_shared)
    elseif(PZIP_BUILD_STATIC)
        target_link_libraries(pzip_example_zstd_c PRIVATE pzip_static)
    endif()
    target_include_directories(pzip_example_zstd_c PRIVATE include)

    add_executable(pzip_example_lz4_c examples/lz4_codec_c.c)
    if(PZIP_BUILD_SHARED)
        target_link_libraries(pzip_example_lz4_c PRIVATE pzip_shared)
    elseif(PZIP_BUILD_STATIC)
        target_link_libraries(pzip_example_lz4_c PRIVATE pzip_static)
    endif()
    target_include_directories(pzip_example_lz4_c PRIVATE include)

    add_executable(pzip_zip examples/pzip_zip_cli.cpp)
    add_executable(pzip_unzip examples/pzip_unzip_cli.cpp)
    target_include_directories(pzip_zip PRIVATE include third_party/filesystem/include)
    target_include_directories(pzip_unzip PRIVATE include)
    if(PZIP_BUILD_STATIC)
        target_link_libraries(pzip_zip PRIVATE pzip_static ${PZIP_EXTRA_LIBS} ${PZIP_ZLIB_STATIC_TARGET})
        target_link_libraries(pzip_unzip PRIVATE pzip_static ${PZIP_EXTRA_LIBS} ${PZIP_ZLIB_STATIC_TARGET})
        if(PZIP_USE_BUNDLED_ZSTD)
            target_link_libraries(pzip_zip PRIVATE ${PZIP_ZSTD_STATIC_TARGET})
            target_link_libraries(pzip_unzip PRIVATE ${PZIP_ZSTD_STATIC_TARGET})
        endif()
    elseif(PZIP_BUILD_SHARED)
        target_link_libraries(pzip_zip PRIVATE pzip_shared)
        target_link_libraries(pzip_unzip PRIVATE pzip_shared)
    endif()
    set_target_properties(pzip_zip PROPERTIES OUTPUT_NAME pzip-zip)
    set_target_properties(pzip_unzip PROPERTIES OUTPUT_NAME pzip-unzip)
    install(TARGETS pzip_zip pzip_unzip RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(BUILD_TESTING)
    add_executable(pzip_test_parallel_pipeline tests/parallel_pipeline_c.c)
    if(PZIP_BUILD_SHARED)
        target_link_libraries(pzip_test_parallel_pipeline PRIVATE pzip_shared)
    elseif(PZIP_BUILD_STATIC)
        target_link_libraries(pzip_test_parallel_pipeline PRIVATE pzip_static)
    endif()
    target_include_directories(pzip_test_parallel_pipeline PRIVATE include)
    add_test(NAME pzip_parallel_pipeline COMMAND pzip_test_parallel_pipeline)
    set_tests_properties(pzip_parallel_pipeline PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

    add_executable(pzip_test_cancel_pipeline tests/cancel_pipeline_cpp.cpp)
    if(PZIP_BUILD_SHARED)
        target_link_libraries(pzip_test_cancel_pipeline PRIVATE pzip_shared)
    elseif(PZIP_BUILD_STATIC)
        target_link_libraries(pzip_test_cancel_pipeline PRIVATE pzip_static)
    endif()
    target_link_libraries(pzip_test_cancel_pipeline PRIVATE Threads::Threads)
    target_include_directories(pzip_test_cancel_pipeline PRIVATE include)
    add_test(NAME pzip_cancel_pipeline COMMAND pzip_test_cancel_pipeline)
    set_tests_properties(pzip_cancel_pipeline PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

    add_executable(pzip_test_codec_failure tests/codec_failure_cpp.cpp)
    if(PZIP_BUILD_SHARED)
        target_link_libraries(pzip_test_codec_failure PRIVATE pzip_shared)
    elseif(PZIP_BUILD_STATIC)
        target_link_libraries(pzip_test_codec_failure PRIVATE pzip_static)
    endif()
    target_include_directories(pzip_test_codec_failure PRIVATE include)
    add_test(NAME pzip_codec_failure COMMAND pzip_test_codec_failure)
    set_tests_properties(pzip_codec_failure PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

    add_executable(pzip_test_archive_open_failure tests/archive_open_failure_c.c)
    if(PZIP_BUILD_SHARED)
        target_link_libraries(pzip_test_archive_open_failure PRIVATE pzip_shared)
    elseif(PZIP_BUILD_STATIC)
        target_link_libraries(pzip_test_archive_open_failure PRIVATE pzip_static)
    endif()
    target_include_directories(pzip_test_archive_open_failure PRIVATE include)
    add_test(NAME pzip_archive_open_failure COMMAND pzip_test_archive_open_failure)
    set_tests_properties(pzip_archive_open_failure PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

    add_executable(pzip_test_backpressure_progress tests/backpressure_progress_cpp.cpp)
    if(PZIP_BUILD_SHARED)
        target_link_libraries(pzip_test_backpressure_progress PRIVATE pzip_shared)
    elseif(PZIP_BUILD_STATIC)
        target_link_libraries(pzip_test_backpressure_progress PRIVATE pzip_static)
    endif()
    target_include_directories(pzip_test_backpressure_progress PRIVATE include)
    add_test(NAME pzip_backpressure_progress COMMAND pzip_test_backpressure_progress)
    set_tests_properties(pzip_backpressure_progress PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        TIMEOUT 20)

    add_executable(pzip_test_writer_mid_failure tests/writer_mid_failure_cpp.cpp)
    if(PZIP_BUILD_SHARED)
        target_link_libraries(pzip_test_writer_mid_failure PRIVATE pzip_shared)
    elseif(PZIP_BUILD_STATIC)
        target_link_libraries(pzip_test_writer_mid_failure PRIVATE pzip_static)
    endif()
    target_include_directories(pzip_test_writer_mid_failure PRIVATE include)
    add_test(NAME pzip_writer_mid_failure COMMAND pzip_test_writer_mid_failure)
    set_tests_properties(pzip_writer_mid_failure PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        TIMEOUT 8)

    add_executable(pzip_test_encryption_pipeline tests/encryption_pipeline_cpp.cpp)
    if(PZIP_BUILD_SHARED)
        target_link_libraries(pzip_test_encryption_pipeline PRIVATE pzip_shared)
    elseif(PZIP_BUILD_STATIC)
        target_link_libraries(pzip_test_encryption_pipeline PRIVATE pzip_static)
    endif()
    target_include_directories(pzip_test_encryption_pipeline PRIVATE include)
    add_test(NAME pzip_encryption_pipeline COMMAND pzip_test_encryption_pipeline)
    set_tests_properties(pzip_encryption_pipeline PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        TIMEOUT 8)

    add_executable(pzip_test_extract_pipeline tests/extract_pipeline_cpp.cpp)
    if(PZIP_BUILD_SHARED)
        target_link_libraries(pzip_test_extract_pipeline PRIVATE pzip_shared)
    elseif(PZIP_BUILD_STATIC)
        target_link_libraries(pzip_test_extract_pipeline PRIVATE pzip_static)
    endif()
    target_include_directories(pzip_test_extract_pipeline PRIVATE include)
    add_test(NAME pzip_extract_pipeline COMMAND pzip_test_extract_pipeline)
    set_tests_properties(pzip_extract_pipeline PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        TIMEOUT 8)
endif()

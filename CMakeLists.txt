# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FILESYSTEM_VERSION LIMIT_COUNT 1)
project(FilesystemAPFS VERSION ${FILESYSTEM_VERSION} LANGUAGES C)
include(CTest)
include(FetchContent)
find_package(OpenSSL REQUIRED)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(infiltratr_common
  GIT_REPOSITORY https://github.com/Infiltrator-Projects/Libraries.git
  GIT_TAG 489388567c436f70754db938ebdc4a07c10b7da0)
FetchContent_MakeAvailable(infiltratr_common)
set_property(DIRECTORY "${infiltratr_common_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h"
  "#define LD_VERSION \"${PROJECT_VERSION}\"\n")

# Only the native APFS on-disk analyser and Common V2 adapter form APFS.so.
# The historical worker/fixture API is compiled exclusively for regression
# tests until its consumers have their own Common V2 implementation.
add_library(apfs-driver SHARED
  native/apfs_native.c
  module/apfs_v2_probe.c)
set_target_properties(apfs-driver PROPERTIES
  PREFIX "" OUTPUT_NAME "APFS" SUFFIX ".so"
  C_VISIBILITY_PRESET hidden)
target_compile_definitions(apfs-driver PRIVATE
  _GNU_SOURCE _FILE_OFFSET_BITS=64 IFS_APFS_V2_PRODUCTION=1)
target_include_directories(apfs-driver PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/native"
  "${CMAKE_CURRENT_SOURCE_DIR}/compat"
  "${infiltratr_common_SOURCE_DIR}/include")
target_compile_options(apfs-driver PRIVATE
  -Wall -Wextra -Wpedantic -Werror -ffunction-sections -fdata-sections)
target_link_options(apfs-driver PRIVATE
  -Wl,-z,defs -Wl,--gc-sections
  "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/module/v2-only.map")
target_link_libraries(apfs-driver PRIVATE InfiltratrCommon::Common OpenSSL::Crypto)

if(BUILD_TESTING)
  # Do NOT install/link this library into the production APFS provider.
  add_library(apfs-legacy-test-support SHARED
    native/apfs_native.c
    native/apfs_worker.c
    module/apfs_module.c
    compat/legacy_services.c
    fixtures/apfs_codec.c
    compat/fixture_bridge.c)
  set_source_files_properties(native/apfs_worker.c PROPERTIES
    COMPILE_DEFINITIONS "main=ifs_apfs_worker_main")
  target_compile_definitions(apfs-legacy-test-support PRIVATE
    _GNU_SOURCE _FILE_OFFSET_BITS=64)
  target_include_directories(apfs-legacy-test-support PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${infiltratr_common_SOURCE_DIR}/include")
  target_compile_options(apfs-legacy-test-support PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(apfs-legacy-test-support PRIVATE
    InfiltratrCommon::Common OpenSSL::Crypto)

  add_executable(apfs-v2-probe-test tests/test_apfs_v2_probe.c)
  target_include_directories(apfs-v2-probe-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${infiltratr_common_SOURCE_DIR}/include")
  target_link_libraries(apfs-v2-probe-test PRIVATE
    apfs-driver InfiltratrCommon::Common)
  target_compile_options(apfs-v2-probe-test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME apfs-native-v2-probe COMMAND apfs-v2-probe-test)

  add_executable(apfs-legacy-compat-test tests/test_apfs_module.c)
  target_include_directories(apfs-legacy-compat-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(apfs-legacy-compat-test PRIVATE apfs-legacy-test-support)
  add_test(NAME apfs-v1-archive-regression COMMAND apfs-legacy-compat-test)

  add_executable(apfs-original-native-test tests/test_apfs_native.c)
  target_include_directories(apfs-original-native-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${infiltratr_common_SOURCE_DIR}/include")
  target_compile_definitions(apfs-original-native-test PRIVATE
    _GNU_SOURCE _FILE_OFFSET_BITS=64)
  target_link_libraries(apfs-original-native-test PRIVATE
    apfs-legacy-test-support OpenSSL::Crypto)
  add_test(NAME apfs-original-native COMMAND apfs-original-native-test)

  add_test(NAME apfs-production-v2-exports
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_v2_exports.sh"
            "$<TARGET_FILE:apfs-driver>")
endif()

install(TARGETS apfs-driver LIBRARY DESTINATION /System/Filesystems)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/APFS.pdf"
  DESTINATION /System/Resources/Documentation
  RENAME Filesystem-APFS.pdf)

cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FILESYSTEM_VERSION LIMIT_COUNT 1)
project(FilesystemAPFS VERSION ${FILESYSTEM_VERSION} LANGUAGES C)
include(CTest)
find_package(OpenSSL REQUIRED)
include(FetchContent)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(infiltratr_common GIT_REPOSITORY https://github.com/Infiltrator-Projects/Libraries.git GIT_TAG b50f493d789b840edea40ea09978d94d751533f8)
FetchContent_MakeAvailable(infiltratr_common)
# Common is a link-time dependency; its development files must not be installed
# by this standalone filesystem package.
set_property(DIRECTORY "${infiltratr_common_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h" "#define LD_VERSION \"${PROJECT_VERSION}\"\n")
add_library(apfs-driver SHARED native/apfs_native.c native/apfs_worker.c module/apfs_module.c module/apfs_v2_probe.c compat/legacy_services.c
 fixtures/apfs_codec.c compat/fixture_bridge.c)
set_source_files_properties(native/apfs_worker.c PROPERTIES COMPILE_DEFINITIONS "main=ifs_apfs_worker_main")
set_target_properties(apfs-driver PROPERTIES PREFIX "" OUTPUT_NAME "APFS" SUFFIX ".so")
target_include_directories(apfs-driver PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/compat" "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include" "${CMAKE_CURRENT_BINARY_DIR}" "${infiltratr_common_SOURCE_DIR}/include")
target_compile_definitions(apfs-driver PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(apfs-driver PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(apfs-driver PRIVATE InfiltratrCommon::Common OpenSSL::Crypto)
if(BUILD_TESTING)
 # Production Common V2 CORE/VOLUME supports qualified read-only mount and
 # metadata query; file I/O, mutation and defrag are not yet advertised.
 add_executable(apfs-v2-probe-test
   tests/test_apfs_v2_probe.c)
 target_link_libraries(apfs-v2-probe-test PRIVATE
   apfs-driver InfiltratrCommon::Common)
 target_include_directories(apfs-v2-probe-test PRIVATE
   "${CMAKE_CURRENT_SOURCE_DIR}/native")
 target_include_directories(apfs-v2-probe-test PRIVATE "${infiltratr_common_SOURCE_DIR}/include")
 target_compile_options(apfs-v2-probe-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
 add_test(NAME apfs-native-v2-probe COMMAND apfs-v2-probe-test)
 # Preserve the original Defragmenter consumer contract during V2 migration.
 add_executable(apfs-legacy-compat-test tests/test_apfs_module.c)
 target_include_directories(apfs-legacy-compat-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
 target_link_libraries(apfs-legacy-compat-test PRIVATE apfs-driver)
 add_test(NAME apfs-v1-consumer-compat COMMAND apfs-legacy-compat-test)
    add_executable(apfs-original-native-test tests/test_apfs_native.c)
    target_include_directories(apfs-original-native-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native" "${infiltratr_common_SOURCE_DIR}/include")
    target_compile_definitions(apfs-original-native-test PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_link_libraries(apfs-original-native-test PRIVATE apfs-driver OpenSSL::Crypto)
    add_test(NAME apfs-original-native COMMAND apfs-original-native-test)
endif()
install(TARGETS apfs-driver LIBRARY DESTINATION /System/Filesystems)
# Install only this filesystem's illustrated PDF beside its native shared object.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/APFS.pdf"
        DESTINATION /System/Resources/Documentation RENAME Filesystem-APFS.pdf)

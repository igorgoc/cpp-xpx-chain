cmake_minimum_required(VERSION 3.5)
project(catapult_server)
include(CMakeGlobalSettings.cmake)
option(ENABLE_CODE_COVERAGE "Enable code coverage" OFF)
option(SANITIZE_ADDRESS "Enable address sanitizer" OFF)
option(SANITIZE_THREAD "Enable thread sanitizer" OFF)
option(SANITIZE_UNDEFINED "Enable undefined behavior sanitizer" OFF)
option(SIRIUS_DRIVE_MULTI "changes for sirius drive-multi" ON)
option(XPX_STORAGE_SDK_NOT_BUILD_EXAMPLES "Not Build XPX-STORAGE SDK examples." ON)

set(CPP_XPX_CHAIN ON)
set(SKIP_SUPERCONTRACT ON)

if (SIRIUS_DRIVE_MULTI)
	add_definitions(-DSIRIUS_DRIVE_MULTI)
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -DDEADLOCK_THRESHOLD_MILLISECONDS=600000 -w")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

message(STATUS "CATAPULT_VERSION_DESCRIPTION: ${CATAPULT_VERSION_DESCRIPTION}")
message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
message(STATUS "CMAKE_PREFIX_PATH: ${CMAKE_PREFIX_PATH}")
message(STATUS "BOOST_ROOT: $ENV{BOOST_ROOT}")

set(CORE_CATAPULT_LIBS catapult.io catapult.ionet catapult.model catapult.thread catapult.utils)

### setup boost

if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
	find_package(Boost COMPONENTS atomic system date_time regex timer chrono log thread filesystem program_options random REQUIRED)
else()
	find_package(Boost COMPONENTS atomic system date_time regex timer chrono log thread filesystem program_options random stacktrace_backtrace REQUIRED)
	add_definitions(-DBOOST_STACKTRACE_USE_BACKTRACE)
	set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DSHOW_BACKTRACE")
endif()

include_directories(SYSTEM ${Boost_INCLUDE_DIR})

### setup openssl
message("--- locating openssl dependencies ---")

find_package(OpenSSL 3.0.2 REQUIRED)
message("OpenSSL   ver: ${OPENSSL_VERSION}")
message("OpenSSL  root: ${OPENSSL_ROOT_DIR}")
message("OpenSSL   inc: ${OPENSSL_INCLUDE_DIR}")
message("OpenSSL  libs: ${OPENSSL_LIBRARIES}")

# used to add openssl dependencies to a target
function(catapult_add_openssl_dependencies TARGET_NAME)
	include_directories(${OPENSSL_INCLUDE_DIR})
	target_link_libraries(${TARGET_NAME} OpenSSL::SSL)
endfunction()

### setup rocksdb
message("--- locating rocksdb dependencies ---")
if (WIN32)
	find_package(RocksDB REQUIRED)
else()
	find_path(ROCKSDB_ROOT_DIR NAMES include/rocksdb/db.h)
	find_library(ROCKSDB_LIBRARIES NAMES rocksdb HINTS ${ROCKSDB_ROOT_DIR})
	find_path(ROCKSDB_INCLUDE_DIR NAMES rocksdb/db.h HINTS ${ROCKSDB_ROOT_DIR}/include)

	message("rocksdb   lib: ${ROCKSDB_LIBRARIES}")
	message("rocksdb   inc: ${ROCKSDB_INCLUDE_DIR}")
endif()

# used to add rocksdb dependencies to a target
function(catapult_add_rocksdb_dependencies TARGET_NAME)
	if (WIN32)
		target_link_libraries(${TARGET_NAME} RocksDB::rocksdb)
	else()
		include_directories(SYSTEM ${ROCKSDB_INCLUDE_DIR})
		target_link_libraries(${TARGET_NAME} ${ROCKSDB_LIBRARIES})
	endif()
endfunction()

### extension settings

### add source directories
add_subdirectory(external)

if(NOT APPLE)
	set(DO_NOT_SKIP_BUILD_TESTS TRUE)
endif()

# run catapult tests first
add_subdirectory(src)
if(DO_NOT_SKIP_BUILD_TESTS)
add_subdirectory(tests)
endif()
# run other tests second
add_subdirectory(sdk)
add_subdirectory(plugins)
add_subdirectory(extensions)

add_subdirectory(resources)

if (EXISTS "${CMAKE_SOURCE_DIR}/internal/CMakeLists.txt")
	message("--- adding internal directory ---")
	add_subdirectory(internal)
endif()

add_subdirectory(tools)

# Help programs
if(DO_NOT_SKIP_BUILD_TESTS)
add_subdirectory(spammer)
endif()

add_subdirectory(cpp-xpx-storage-sdk)

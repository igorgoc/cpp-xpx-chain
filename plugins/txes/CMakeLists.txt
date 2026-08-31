cmake_minimum_required(VERSION 3.2)

function(catapult_tx_plugin_src PLUGIN_BASE_NAME)
	# create an sdk lib
	if (DEFINED PLUGIN_SDK_FOLDERS)
		catapult_library_target(${PLUGIN_BASE_NAME}.sdk ${PLUGIN_SDK_FOLDERS})
		target_link_libraries(${PLUGIN_BASE_NAME}.sdk catapult.model)
	endif()

	# create a deps lib
	if (DEFINED PLUGIN_DEPS_FOLDERS)
		catapult_library_target(${PLUGIN_BASE_NAME}.deps ${PLUGIN_DEPS_FOLDERS})
		target_link_libraries(${PLUGIN_BASE_NAME}.deps ${PLUGIN_CATAPULT_LIBS} ${CATAPULT_BOOST_LIBRARIES})
	endif()

	if (DEFINED PLUGIN_SDK_FOLDERS)
		target_link_libraries(${PLUGIN_BASE_NAME}.deps ${PLUGIN_BASE_NAME}.sdk)
	endif()

	# create a plugin dll
	catapult_shared_library_target(${PLUGIN_BASE_NAME} plugins)
	target_link_libraries(${PLUGIN_BASE_NAME} ${PLUGIN_CATAPULT_LIBS} ${CATAPULT_BOOST_LIBRARIES})

	if (DEFINED PLUGIN_DEPS_FOLDERS)
		target_link_libraries(${PLUGIN_BASE_NAME} ${PLUGIN_BASE_NAME}.deps)
	endif()
endfunction()

function(catapult_tx_plugin_tests TARGET_NAME)
	string(REGEX MATCH "\.[a-z_]+$" PLUGIN_NAME ${TARGET_NAME})
	string(SUBSTRING ${PLUGIN_NAME} 1 -1 PLUGIN_NAME)

	if (NOT TARGET "tests.catapult.test.plugins.${PLUGIN_NAME}")
		set(PLUGIN_TEST_FOLDERS test ${PLUGIN_TEST_FOLDERS})
	endif()

	catapult_test_executable_target(${TARGET_NAME} cache ${PLUGIN_SDK_FOLDERS} ${PLUGIN_DEPS_FOLDERS} ${PLUGIN_TEST_FOLDERS} plugins)

	if (TARGET "tests.catapult.test.plugins.${PLUGIN_NAME}")
		target_link_libraries(${TARGET_NAME} tests.catapult.test.plugins.${PLUGIN_NAME})
	endif()

	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}" PARENT_SCOPE)
endfunction()

add_subdirectory(account_link)
add_subdirectory(aggregate)
add_subdirectory(config)
add_subdirectory(committee)
add_subdirectory(contract)
add_subdirectory(exchange)
add_subdirectory(exchange_sda)
add_subdirectory(lock_hash)
add_subdirectory(lock_secret)
add_subdirectory(lock_shared)
add_subdirectory(metadata)
add_subdirectory(metadata_v2)
add_subdirectory(mosaic)
add_subdirectory(multisig)
add_subdirectory(namespace)
add_subdirectory(operation)
add_subdirectory(property)
add_subdirectory(transfer)
add_subdirectory(upgrade)
add_subdirectory(service)
add_subdirectory(storage)
add_subdirectory(supercontract)
add_subdirectory(streaming)
add_subdirectory(liquidityprovider)
add_subdirectory(dbrb)

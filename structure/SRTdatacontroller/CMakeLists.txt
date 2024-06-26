project(srtdatacontroller)

option(ENABLE_CXX17 "Should the c++17 parts (file receive/send) be enabled" OFF)

#AUX_SOURCE_DIRECTORY(SOURCES ./) 
FILE(GLOB SOURCES *.cpp *.hpp)

add_executable(srtdatacontroller ${SOURCES})

message(STATUS "SRT_INCLUDE_DIR: ${SRT_INCLUDE_DIR}")
message(STATUS "SRT_UTILS_INCLUDE_DIR: ${SRT_UTILS_INCLUDE_DIR}")
message(STATUS "CMAKE_BINARY_DIR: ${CMAKE_BINARY_DIR}")
message(STATUS "VIRTUAL_srtsupport: ${VIRTUAL_srtsupport}")
message(STATUS "SSL_LIBRARY_DIRS: ${SSL_LIBRARY_DIRS}")
target_include_directories(srtdatacontroller PUBLIC
	${SRT_INCLUDE_DIR}
	${SRT_UTILS_INCLUDE_DIR}
	${PTHREAD_INCLUDE_DIR}
	${SSL_INCLUDE_DIRS}
	)

target_link_directories(srtdatacontroller PUBLIC ${SSL_LIBRARY_DIRS})

if (ENABLE_BONDING OR ENABLE_EXPERIMENTAL_BONDING)
	target_compile_options(srtdatacontroller
		PRIVATE "-DENABLE_BONDING=1" "-DENABLE_EXPERIMENTAL_BONDING"
	)
endif()

if (ENABLE_AEAD_API_PREVIEW)
	target_compile_options(srtdatacontroller
		PRIVATE "-DENABLE_AEAD_API_PREVIEW"
	)
endif()

if (ENABLE_MAXREXMITBW)
	target_compile_options(srtdatacontroller
		PRIVATE "-DENABLE_MAXREXMITBW=1"
	)
endif()

if (ENABLE_CXX17)
	set(REQUIRE_CXX_VER 17)
	if (${CMAKE_CXX_COMPILER_ID} MATCHES "GNU|Intel")
		target_compile_options(srtdatacontroller PRIVATE "-lstdc++fs")
		set(LINKSTDCPP_FS stdc++fs)
	endif()
	target_compile_options(srtdatacontroller PRIVATE "-DENABLE_FILE_TRANSFER" "-DENABLE_CXX17")
	message(STATUS "C++17 features: ENABLED")
else()
	set(REQUIRE_CXX_VER 14)
	message(STATUS "C++17 features: DISABLED")
endif()

set (VIRTUAL_srtsupport $<TARGET_OBJECTS:srtsupport_virtual>)
#spdlog_enable_warnings(srtdatacontroller)
target_link_libraries(srtdatacontroller
	PRIVATE CLI11::CLI11
	PRIVATE spdlog::spdlog
	PRIVATE ${TARGET_srt}_static ${VIRTUAL_srtsupport} ${LINKSTDCPP_FS}
	PRIVATE nlohmann_json::nlohmann_json)



set_target_properties(srtdatacontroller
	PROPERTIES
	CXX_STANDARD ${REQUIRE_CXX_VER}
	ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)


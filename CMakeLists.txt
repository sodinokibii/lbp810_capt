cmake_minimum_required(VERSION 3.21)
project(CanonLBP810WinSpoolPrototype LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(LIBCAPT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBCAPT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libcapt
	GIT_REPOSITORY https://github.com/darkvision77/libcapt.git
	GIT_TAG 8f1fd502e6fee893d409f09c5895248c189ead4
)
FetchContent_MakeAvailable(libcapt)

add_executable(lbp810_spool src/main.cpp)
target_compile_definitions(lbp810_spool PRIVATE UNICODE _UNICODE)
target_link_libraries(lbp810_spool PRIVATE Winspool libcapt)
if(MSVC)
	target_link_libraries(lbp810_spool PRIVATE Setupapi)
endif()

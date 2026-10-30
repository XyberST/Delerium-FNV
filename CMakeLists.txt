cmake_minimum_required(VERSION 3.20)
project(DeliriumFNV CXX)
# Must be a 32-bit build: configure with  cmake -S . -B build -A Win32
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "FalloutNV.exe is 32-bit. Configure with -A Win32.")
endif()
set(CMAKE_CXX_STANDARD 17)
add_library(DeliriumFNV SHARED src/main.cpp src/probe.cpp src/engine.cpp src/log.cpp src/ingame.cpp src/actor.cpp src/observe.cpp)
target_include_directories(DeliriumFNV PRIVATE src)
target_compile_definitions(DeliriumFNV PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
set_target_properties(DeliriumFNV PROPERTIES PREFIX "" MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Stage the plugin the way the game wants it:  Data/NVSE/Plugins/DeliriumFNV.dll  +  Data/NVSE/Plugins/DeliriumFNV/*
add_custom_command(TARGET DeliriumFNV POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/stage/Data/NVSE/Plugins/DeliriumFNV
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:DeliriumFNV> ${CMAKE_BINARY_DIR}/stage/Data/NVSE/Plugins/
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/stage/Data/NVSE/Plugins/DeliriumFNV)

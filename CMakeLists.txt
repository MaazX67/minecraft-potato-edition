cmake_minimum_required(VERSION 3.21)

# Static CRT: the exe must run on a bare Windows 10 box. The target laptop's
# MSVCP140.dll (14.32) predates the VS2022 17.10 mutex layout change and
# crashes with newer-toolset binaries; linking the CRT in avoids the redist
# entirely and keeps the shipped exe self-contained.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(MinecraftPotatoEdition LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /EHsc /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Embed the HLSL source so the shipped exe is fully self-contained.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/shader.fx POTATO_SHADER_SRC)
configure_file(src/shader_embed.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/shader_embed.h @ONLY)

# CPU-only core: world gen, greedy meshing, asset/atlas logic, combat math,
# ring queue, particles. No D3D dependency, so the headless selftest links it
# without a graphics device.
add_library(potato_core STATIC src/world.cpp src/assets.cpp src/engine.cpp)
target_include_directories(potato_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(potato_core SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

add_executable(PotatoEdition WIN32 src/main.cpp src/renderer.cpp src/gui.cpp src/sound.cpp)
target_include_directories(PotatoEdition PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(PotatoEdition PRIVATE potato_core d3d10 dxgi d3dcompiler dxguid xaudio2)

add_executable(PotatoSelfTest src/selftest.cpp)
target_include_directories(PotatoSelfTest PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(PotatoSelfTest PRIVATE potato_core d3dcompiler)
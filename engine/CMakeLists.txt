cmake_minimum_required(VERSION 3.16)
project(steel_horizon_rts C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Lockstep multiplayer needs bit-identical floating point on every peer: no fused
# multiply-adds that one build would use and another wouldn't.
add_compile_options(-ffp-contract=off)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

# Everything except main.c and render/gl, so the simulation can be tested headless.
add_library(shcore STATIC src/asset.c src/data.c src/json.c src/sim.c src/combat.c src/layout.c src/ai.c src/air.c src/damage.c src/captain.c src/base.c src/fleet.c src/command.c src/net.c src/genship.c src/vision.c src/islands.c src/synth.c src/specials.c src/commander.c)
target_compile_options(shcore PRIVATE -Wall -Wextra)
target_link_libraries(shcore PUBLIC m)

add_executable(shrts src/main.c src/render.c src/gl.c src/hud.c src/audio.c src/menu.c)
target_compile_options(shrts PRIVATE -Wall -Wextra)
target_link_libraries(shrts PRIVATE shcore PkgConfig::SDL2)

enable_testing()
add_executable(test_engine tests/test_engine.c)
target_include_directories(test_engine PRIVATE src)
target_link_libraries(test_engine PRIVATE shcore)
add_test(NAME engine COMMAND test_engine ${CMAKE_SOURCE_DIR}/../assets)

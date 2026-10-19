# RIZOMA — Build configuration
# Copyright (C) 2026 José Pedro Trindade
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.25)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

project(morfo VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(TIFF REQUIRED)
find_path(BOOST_INCLUDE_DIR NAMES boost/asio.hpp REQUIRED)

add_library(morfo_roots_exp003 STATIC
 profiles/roots/exp003/src/core.cpp
 profiles/roots/exp003/src/image_io.cpp
 profiles/roots/exp003/src/comparison.cpp)
target_include_directories(morfo_roots_exp003 PUBLIC ${CMAKE_SOURCE_DIR}/profiles/roots/exp003/include)
target_link_libraries(morfo_roots_exp003 PUBLIC TIFF::TIFF PNG::PNG JPEG::JPEG OpenSSL::Crypto)
target_compile_options(morfo_roots_exp003 PRIVATE -Wall -Wextra -Wpedantic)

add_library(morfo_root_metrics STATIC src/morfo_root_metrics.cpp)
target_include_directories(morfo_root_metrics PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(morfo_root_metrics PRIVATE -Wall -Wextra -Wpedantic)

add_executable(morfo-server src/main.cpp src/database.cpp src/analysis.cpp src/tiles.cpp src/learning.cpp src/morfo_profiles.cpp)
target_link_libraries(morfo-server PRIVATE morfo_roots_exp003 morfo_root_metrics)
target_include_directories(morfo-server PRIVATE ${BOOST_INCLUDE_DIR})
target_link_libraries(morfo-server PRIVATE Threads::Threads OpenSSL::Crypto SQLite3::SQLite3 PNG::PNG JPEG::JPEG TIFF::TIFF)
target_compile_definitions(morfo-server PRIVATE BOOST_ERROR_CODE_HEADER_ONLY BOOST_SYSTEM_NO_DEPRECATED)
target_compile_options(morfo-server PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
find_package(Python3 COMPONENTS Interpreter)
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
  add_test(NAME rizoma.http-smoke COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/smoke.py $<TARGET_FILE:morfo-server> ${CMAKE_SOURCE_DIR}/web)
  set_tests_properties(rizoma.http-smoke PROPERTIES TIMEOUT 90)
  add_test(NAME rizoma.sqlite-migrations COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/migrations.py $<TARGET_FILE:morfo-server>)
  set_tests_properties(rizoma.sqlite-migrations PROPERTIES TIMEOUT 30)
endif()

add_executable(morfo-roots-golden tests/morfo_roots_golden.cpp)
target_link_libraries(morfo-roots-golden PRIVATE morfo_roots_exp003)
if(BUILD_TESTING)
 add_executable(morfo-root-metrics-001c-tests tests/morfo_root_metrics_001a.cpp)
 target_link_libraries(morfo-root-metrics-001c-tests PRIVATE morfo_root_metrics)
 if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(morfo-root-metrics-001c-tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
 endif()
 add_test(NAME morfo.root-metrics.001c.topology COMMAND morfo-root-metrics-001c-tests)

 # Teste inteiramente sintetico, disponivel em qualquer clone do MORFO.
 add_executable(morfo-roots-core-tests profiles/roots/exp003/tests/core_tests.cpp)
 target_link_libraries(morfo-roots-core-tests PRIVATE morfo_roots_exp003)
 if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(morfo-roots-core-tests PRIVATE -UNDEBUG)
 endif()
 add_test(NAME morfo.roots.exp003.core COMMAND morfo-roots-core-tests)

 # A imagem cientifica real e privada. Nao deve entrar no Git publico.
 if(EXISTS "${CMAKE_SOURCE_DIR}/tests/fixtures/private/monolito_rgb.png" AND
    EXISTS "${CMAKE_SOURCE_DIR}/tests/fixtures/private/exp003_expected_mask.png")
  add_test(NAME morfo.roots.exp003.golden
   COMMAND morfo-roots-golden
    ${CMAKE_SOURCE_DIR}/tests/fixtures/private/monolito_rgb.png
    ${CMAKE_SOURCE_DIR}/tests/fixtures/private/exp003_expected_mask.png)
 else()
  message(STATUS "EXP-003 golden privado: nao registrado (fixtures ausentes)")
 endif()
endif()

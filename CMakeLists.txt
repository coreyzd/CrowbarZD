cmake_minimum_required(VERSION 3.20)
project(CrowbarZD VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CZD_BUILD_APP "Build the Qt 6 application" ON)
option(CZD_SANITIZE  "Build with address + undefined behaviour sanitizers" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

add_library(czd_warnings INTERFACE)
target_compile_options(czd_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wshadow>)

if(CZD_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(czd_core STATIC
  src/core/readers.cpp
  src/core/decompile.cpp
  src/core/compile.cpp
  src/core/batch.cpp)
target_include_directories(czd_core PUBLIC src/core/include)
target_link_libraries(czd_core PRIVATE czd_warnings)

add_executable(crowbarzd-cli src/cli/main.cpp)
target_link_libraries(crowbarzd-cli PRIVATE czd_core czd_warnings)

if(CZD_BUILD_APP)
  find_package(Qt6 6.4 COMPONENTS Core Gui Widgets Concurrent QUIET)
  if(Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)
    add_executable(crowbarzd src/app/main.cpp src/app/MainWindow.cpp src/app/MainWindow.hpp)
    target_compile_definitions(crowbarzd PRIVATE CZD_VERSION="${PROJECT_VERSION}")
    target_link_libraries(crowbarzd PRIVATE czd_core czd_warnings Qt6::Widgets Qt6::Concurrent)
  else()
    message(WARNING "Qt 6.4+ not found: skipping the application (crowbarzd-cli still builds)")
  endif()
endif()

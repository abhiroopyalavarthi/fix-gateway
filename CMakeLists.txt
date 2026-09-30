cmake_minimum_required(VERSION 3.20)
project(fix_gateway CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# --- QuickFIX (built from source, pinned) -------------------------------------
set(QUICKFIX_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QUICKFIX_TESTS OFF CACHE BOOL "" FORCE)
set(QUICKFIX_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(HAVE_SSL OFF CACHE BOOL "" FORCE)
# QuickFIX writes its libs to ${CMAKE_SOURCE_DIR}/lib by default, which is our repo
set(QUICKFIX_LIB_OUTPUT_DIR ${CMAKE_BINARY_DIR}/quickfix-lib CACHE PATH "" FORCE)
FetchContent_Declare(quickfix
  GIT_REPOSITORY https://github.com/quickfix/quickfix.git
  GIT_TAG 386ce46e917ae494ab6e90b1be90fd421cdbe3f9)
FetchContent_MakeAvailable(quickfix)
# QuickFIX only puts its headers under include/quickfix/ at install time; point
# a quickfix/ directory at the sources so we can write #include "quickfix/Session.h"
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/qf-include)
file(CREATE_LINK ${quickfix_SOURCE_DIR}/src/C++ ${CMAKE_BINARY_DIR}/qf-include/quickfix SYMBOLIC)
# (a wrapper target, because QuickFIX exports its own target for install)
add_library(qf INTERFACE)
target_link_libraries(qf INTERFACE quickfix)
target_include_directories(qf SYSTEM INTERFACE ${CMAKE_BINARY_DIR}/qf-include)
target_compile_options(quickfix PRIVATE -w)  # its own warnings aren't ours to fix
set_target_properties(quickfix PROPERTIES   # ...and neither are the ones from its headers
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES $<TARGET_PROPERTY:quickfix,INTERFACE_INCLUDE_DIRECTORIES>)

# --- order book (git submodule) ----------------------------------------------
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/orderbook/src/FastOrderBook.cpp)
  message(FATAL_ERROR "orderbook/ is empty, run: git submodule update --init")
endif()
add_library(orderbook STATIC orderbook/src/FastOrderBook.cpp)
target_include_directories(orderbook PUBLIC orderbook/include)

# --- gateway core (no FIX) -----------------------------------------------------
add_library(gateway_core STATIC src/OrderHandler.cpp src/RiskChecks.cpp)
target_include_directories(gateway_core PUBLIC include)
target_link_libraries(gateway_core PUBLIC orderbook)
target_compile_options(gateway_core PRIVATE -Wall -Wextra)

# --- FIX layer -----------------------------------------------------------------
add_library(gateway_fix STATIC src/FixConvert.cpp src/Gateway.cpp client/FixClient.cpp)
target_include_directories(gateway_fix PUBLIC include client)
target_link_libraries(gateway_fix PUBLIC gateway_core qf)
target_compile_options(gateway_fix PRIVATE -Wall -Wextra)

add_executable(fix_gateway src/main.cpp)
target_link_libraries(fix_gateway gateway_fix)

add_executable(fix_client client/main.cpp)
target_link_libraries(fix_client gateway_fix)

# --- fixlog (plain C++, no QuickFIX) ------------------------------------------
add_library(fixlog_lib STATIC tools/fixlog/FixLog.cpp)
target_include_directories(fixlog_lib PUBLIC tools/fixlog)
target_compile_options(fixlog_lib PRIVATE -Wall -Wextra)
add_executable(fixlog tools/fixlog/main.cpp)
target_link_libraries(fixlog fixlog_lib)

# --- tests ---------------------------------------------------------------------
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(tests
  tests/test_price.cpp
  tests/test_risk.cpp
  tests/test_order_handler.cpp
  tests/test_fix_convert.cpp
  tests/test_fixlog.cpp
  tests/test_session.cpp)
target_link_libraries(tests gateway_fix fixlog_lib GTest::gtest_main)
target_compile_definitions(tests PRIVATE
  SPEC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/config/spec"
  CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/config")
include(GoogleTest)
gtest_discover_tests(tests DISCOVERY_TIMEOUT 60)

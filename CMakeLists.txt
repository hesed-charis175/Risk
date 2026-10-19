cmake_minimum_required(VERSION 3.10)
project(risk LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")
add_executable(riskc
    src/driver.cpp
    src/lexer.cpp
    src/parser.cpp
    src/typechecker.cpp
    src/codegen.cpp
)
target_include_directories(riskc PRIVATE src)

# LSP (risk-lsp/). You can configure with -DRISK_BUILD_LSP=OFF to skip it.
option(RISK_BUILD_LSP "Build the risk-lsp language server" ON)
if(RISK_BUILD_LSP AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/risk-lsp/CMakeLists.txt")
  add_subdirectory(risk-lsp)
endif()

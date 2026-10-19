cmake_minimum_required(VERSION 3.10)

# It works two ways:
#   - either as part of the main Risk build (the root CMakeLists), or
#   - on its own ( cmake -S risk-lsp -B risk-lsp/build && cmake --build risk-lsp/build )
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(risk_lsp LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
  endif()
  set(CMAKE_CXX_FLAGS_RELEASE "-O2")
endif()

# It compiles riskc's front-end (lexer, parser, type checker) from ../src, so editor diagnostics always match riskc.
set(RISK_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src" CACHE PATH "Risk compiler sources")

add_executable(risk-lsp
    src/server.cpp
    src/model.cpp
    src/analyzer.cpp
    src/builtins.cpp
    ${RISK_SRC_DIR}/lexer.cpp
    ${RISK_SRC_DIR}/parser.cpp
    ${RISK_SRC_DIR}/typechecker.cpp
)
target_include_directories(risk-lsp PRIVATE src ${RISK_SRC_DIR})

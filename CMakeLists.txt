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

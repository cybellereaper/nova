cmake_minimum_required(VERSION 3.20)
project(nova LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(GLOB NOVA_SRC CONFIGURE_DEPENDS src/*.cpp)

add_library(nova_core STATIC ${NOVA_SRC})
target_include_directories(nova_core PUBLIC include)
target_compile_options(nova_core PRIVATE -Wall -Wextra -Wpedantic -fpermissive)

function(add_nova_tool target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE nova_core)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -fpermissive)
endfunction()

add_nova_tool(nova-fmt tools/nova_fmt.cpp)
add_nova_tool(nova-repl tools/nova_repl.cpp)
add_nova_tool(nova-lsp tools/nova_lsp.cpp)
add_nova_tool(nova-new tools/nova_new.cpp)
add_nova_tool(nova-check tools/nova_check.cpp)

include(CTest)
if(BUILD_TESTING)
  add_executable(nova_tests tests/parser_tests.cpp)
  target_link_libraries(nova_tests PRIVATE nova_core)
  target_compile_options(nova_tests PRIVATE -Wall -Wextra -Wpedantic -fpermissive)
  add_test(NAME parser_tests COMMAND nova_tests)
  set_tests_properties(parser_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

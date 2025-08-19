cmake_minimum_required(VERSION 3.16)
project(netchat LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-O2 -Wall -Wextra)

find_package(Boost REQUIRED COMPONENTS system)

add_executable(netchat src/server.cpp)
target_link_libraries(netchat PRIVATE Boost::system pthread)

cmake_minimum_required(VERSION 3.20)
project(tss LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -Wall -Wextra -fsanitize=address")
set(CMAKE_C_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

add_subdirectory(src)
add_subdirectory(tests)

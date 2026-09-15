cmake_minimum_required(VERSION 3.20)
project(LSM_Engine VERSION 0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

add_library(lsm_storage
        src/wal_writer.cpp
        src/memtable.cpp
        src/engine.cpp
        src/sstable_builder.cpp
        src/sstable_reader.cpp
)

target_include_directories(lsm_storage PUBLIC include)
target_link_libraries(lsm_storage
        PRIVATE ZLIB::ZLIB
        PUBLIC Threads::Threads
)

add_executable(lsm_main main.cpp)
target_link_libraries(lsm_main PRIVATE lsm_storage)

add_executable(lsm_client src/client.cpp)
target_link_libraries(lsm_client PRIVATE Threads::Threads)

enable_testing()
add_executable(lsm_tests tests/test_engine.cpp)
target_link_libraries(lsm_tests PRIVATE lsm_storage)
add_test(NAME engine COMMAND lsm_tests)

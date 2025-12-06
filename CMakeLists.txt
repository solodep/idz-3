cmake_minimum_required(VERSION 3.16)
project(inventory_ipc CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(inventory_v1   src/inventory_v1.cpp)
add_executable(inventory_init src/inventory_init.cpp)
add_executable(worker         src/worker.cpp)
add_executable(observer       src/observer.cpp)

foreach(t inventory_v1 inventory_init worker observer)
    target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(${t} PRIVATE Threads::Threads rt)
endforeach()

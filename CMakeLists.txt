cmake_minimum_required(VERSION 3.30)
project(HTTP_Server C)

set(CMAKE_C_STANDARD 11)

add_executable(HTTP_Server main.c
        server.c
        threadpool.c
        server_test.c)

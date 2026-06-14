cmake_minimum_required(VERSION 3.10)
project(tsched C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

add_compile_options(-Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
# add_link_options was introduced after the declared CMake 3.10 baseline.
set(CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")

include_directories(include)

add_library(tsched_core
    src/config.c
    src/heap.c
    src/log.c
    src/util.c)

add_executable(tschedd src/daemon.c)
target_link_libraries(tschedd tsched_core)

add_executable(tschedctl src/client.c)
target_link_libraries(tschedctl tsched_core)

enable_testing()
add_executable(test_heap tests/test_heap.c src/heap.c)
add_test(NAME heap COMMAND test_heap)
add_executable(test_config tests/test_config.c src/config.c src/util.c)
add_test(NAME config COMMAND test_config)
add_executable(test_ipc tests/test_ipc.c)
add_executable(test_log tests/test_log.c src/log.c src/config.c src/util.c)
add_test(NAME log COMMAND test_log)
add_test(NAME integration
    COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_integration.sh
            $<TARGET_FILE:tschedd> $<TARGET_FILE:tschedctl>
            $<TARGET_FILE:test_ipc>)
add_test(NAME capacity
    COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_capacity.sh
            $<TARGET_FILE:tschedd> $<TARGET_FILE:tschedctl>)
add_test(NAME timing
    COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_timing.sh
            $<TARGET_FILE:tschedd> $<TARGET_FILE:tschedctl>)
add_test(NAME backpressure
    COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_backpressure.sh
            $<TARGET_FILE:tschedd> $<TARGET_FILE:test_ipc>)
add_test(NAME output_backpressure
    COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_output_backpressure.sh
            $<TARGET_FILE:tschedd> $<TARGET_FILE:tschedctl>)

install(TARGETS tschedd tschedctl RUNTIME DESTINATION bin)
install(FILES etc/tsched.conf etc/tasks.conf DESTINATION etc/tsched)
install(FILES www/index.html www/app.js www/style.css DESTINATION share/tsched/www)

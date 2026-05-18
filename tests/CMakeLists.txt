find_package(MPI REQUIRED COMPONENTS C)

add_executable(test_ybus test_ybus.c)
target_include_directories(test_ybus PRIVATE ${MPI_C_INCLUDE_PATH})
target_link_libraries(test_ybus PRIVATE transient ${MPI_C_LIBRARIES})

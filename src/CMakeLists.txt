find_package(MPI REQUIRED COMPONENTS C)

set(LIBSOURCES
    arena.c
    dynarray.c
    hash.c
    csv.c
    log.c
    parser/raw.c
    parser/dyr.c
    parser/ini.c
    ybus.c
    powerflow.c
    machine.c
    dae.c
    integrator.c
    events.c
    output.c
    plot.c
)

add_library(transient STATIC ${LIBSOURCES})
target_include_directories(transient PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MPI_C_INCLUDE_PATH}
    /usr/include/suitesparse
)
target_link_libraries(transient PUBLIC
    sundials_ida
    sundials_core
    sundials_nvecserial
    klu
    m
    lapacke
)

add_executable(transient-cli ../cli/main.c)
target_include_directories(transient-cli PRIVATE ${MPI_C_INCLUDE_PATH})
target_link_libraries(transient-cli PRIVATE transient ${MPI_C_LIBRARIES})

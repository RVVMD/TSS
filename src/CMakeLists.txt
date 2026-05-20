find_package(MPI REQUIRED COMPONENTS C)

# GTK3
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

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
    comtrade.c
    output.c
)

add_library(transient STATIC ${LIBSOURCES})
target_include_directories(transient PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MPI_C_INCLUDE_PATH}
    /usr/include/suitesparse
    ${GTK3_INCLUDE_DIRS}
)
target_link_libraries(transient PUBLIC
    sundials_ida
    sundials_core
    sundials_nvecserial
    klu
    m
    lapacke
    ${GTK3_LIBRARIES}
)

add_executable(transient-cli ../cli/main.c)
target_include_directories(transient-cli PRIVATE ${MPI_C_INCLUDE_PATH})
target_link_libraries(transient-cli PRIVATE transient ${MPI_C_LIBRARIES})

add_executable(transient-gui ../gui/gui.c)
target_include_directories(transient-gui PRIVATE ${MPI_C_INCLUDE_PATH})
target_link_libraries(transient-gui PRIVATE transient ${MPI_C_LIBRARIES} ${GTK3_LIBRARIES})
target_compile_options(transient-gui PRIVATE ${GTK3_CFLAGS_OTHER})

cmake_minimum_required(VERSION 3.13)
project(ecall_tcu C)

#
# eCall TCU - baseline v1
#
# Embedded Linux C, C99, MISRA C:2012 style. Strict warnings are errors.
#

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # needed for POSIX/SocketCAN/strtok_r etc.

add_compile_options(-Wall -Wextra -Werror -pedantic)

# POSIX feature macros for clock_gettime, strtok_r, O_NONBLOCK, CAN headers.
add_compile_definitions(_GNU_SOURCE)

enable_testing()

# ---- common ---------------------------------------------------------------
add_library(ecall_common STATIC
    ap/libs/common/ecall_status.c)
target_include_directories(ecall_common PUBLIC ap/libs/common)
target_link_libraries(ecall_common PUBLIC rt)   # clock_gettime on older libc

# ---- gnss_if --------------------------------------------------------------
add_library(gnss_if STATIC ap/libs/gnss_if/gnss_if.c)
target_include_directories(gnss_if PUBLIC ap/libs/gnss_if)
target_link_libraries(gnss_if PUBLIC ecall_common)

# ---- can_if ---------------------------------------------------------------
add_library(can_if STATIC ap/libs/can_if/can_if.c)
target_include_directories(can_if PUBLIC ap/libs/can_if)
target_link_libraries(can_if PUBLIC ecall_common)

# ---- ecall_core -----------------------------------------------------------
add_library(ecall_core STATIC ap/libs/ecall_core/ecall_core.c)
target_include_directories(ecall_core PUBLIC ap/libs/ecall_core)
target_link_libraries(ecall_core PUBLIC ecall_common can_if)

# ---- msd ------------------------------------------------------------------
add_library(msd STATIC ap/libs/msd/msd.c)
target_include_directories(msd PUBLIC ap/libs/msd)
target_link_libraries(msd PUBLIC ecall_common m)

# ---- psap_if --------------------------------------------------------------
add_library(psap_if STATIC ap/libs/psap_if/psap_if.c)
target_include_directories(psap_if PUBLIC ap/libs/psap_if)
target_link_libraries(psap_if PUBLIC ecall_common)

# ---- ecalld service -------------------------------------------------------
add_executable(ecalld
    ap/services/ecalld/ecalld.c
    ap/services/ecalld/ecalld_config.c)
target_include_directories(ecalld PRIVATE ap/services/ecalld)
target_link_libraries(ecalld
    PRIVATE gnss_if can_if ecall_core msd psap_if ecall_common)

install(TARGETS ecalld RUNTIME DESTINATION bin)
install(FILES ap/services/ecalld/ecalld.conf
        DESTINATION /etc/ecalld)
install(FILES ap/services/ecalld/ecalld.service
        DESTINATION /lib/systemd/system)

# ---- tests (Unity via ctest) ---------------------------------------------
add_library(unity STATIC third_party/unity/unity.c)
target_include_directories(unity PUBLIC third_party/unity)

function(ecall_add_test name)
    add_executable(${name} ap/tests/${name}.c)
    target_link_libraries(${name} PRIVATE unity ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

ecall_add_test(test_gnss gnss_if)
ecall_add_test(test_can  can_if)
ecall_add_test(test_core ecall_core)
ecall_add_test(test_msd  msd)
ecall_add_test(test_psap psap_if)

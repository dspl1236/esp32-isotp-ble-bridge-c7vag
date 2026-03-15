cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(isotp_ble_bridge)

# Build profile selection:
#   cmake .. -DPROFILE=C7_VAG    -> Audi C7 A6/A7/A8 (J533 gateway, Simos8.5 ECU, 8 containers)
#   cmake .. -DPROFILE=MQB       -> Original MQB Simos18/DQ250 tuning profile (default)
#
# Or set via idf.py: idf.py build -DPROFILE=C7_VAG
if(DEFINED PROFILE AND PROFILE STREQUAL "C7_VAG")
    message(STATUS "Building with profile: C7_VAG (Audi A6/A7/A8 C7 platform)")
    add_compile_definitions(PROFILE_C7_VAG)
else()
    message(STATUS "Building with profile: MQB (default Simos18/DQ250)")
endif()

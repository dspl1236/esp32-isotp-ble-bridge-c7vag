cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(isotp_bridge)

# Enable C7 VAG profile — J533/J255/J136/J521/J104/ZF8HP ISO-TP links
add_compile_definitions(PROFILE_C7_VAG)

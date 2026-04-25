cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Point at the host stub library *as a single component* (not at its
# parent — that would pull in slave/ etc. as accidental components).
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../esp_hosted_open"
)
# vendor/esp_hosted/ comes in via idf_component.yml override_path

project(host_p4_demo)

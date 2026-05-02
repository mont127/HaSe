cmake_minimum_required(VERSION 3.20)

project(HaSeCheeseBridge
    VERSION 0.1.0
    DESCRIPTION "CheeseBridge guest Vulkan to macOS host bridge prototypes"
    LANGUAGES C
)

option(CHEESEBRIDGE_BUILD_DEMO "Build the Phase 1 fake CheeseBridge host/guest demo tools" ON)
option(CHEESEBRIDGE_BUILD_GUEST "Build the Linux Vulkan ICD guest stub" OFF)

add_subdirectory(protocol)

if (CHEESEBRIDGE_BUILD_DEMO)
    add_subdirectory(demo)
endif()

if (CHEESEBRIDGE_BUILD_GUEST)
    add_subdirectory(guest)
endif()

# CheeseBridge host: macOS-only Vulkan dispatch process.
#
# Loads MoltenVK at runtime via dlopen, listens on a TCP port for guest ICD
# connections, and replays Vulkan commands the guest sends across the wire.

if (NOT APPLE)
    message(STATUS "CheeseBridge host is macOS-only; skipping")
    return()
endif()

enable_language(OBJCXX)

find_path(VULKAN_HEADERS_INCLUDE_DIR vulkan/vulkan.h
    HINTS
        /usr/local/include
        /opt/homebrew/include
        /opt/homebrew/opt/vulkan-headers/include
        /usr/local/opt/vulkan-headers/include
        $ENV{VULKAN_SDK}/include
)

if (NOT VULKAN_HEADERS_INCLUDE_DIR)
    message(WARNING
        "Vulkan headers not found; cheesebridge_host will not be built. "
        "Install with: brew install vulkan-headers vulkan-loader molten-vk")
    return()
endif()

add_executable(cheesebridge_host
    src/host_main.mm
    src/host_dispatch.mm
    src/host_log.mm
    src/host_moltenvk.mm
    src/host_replay.mm
    src/host_reply.mm
    src/host_resource_table.mm
    src/host_window.mm
)

set_target_properties(cheesebridge_host PROPERTIES
    OBJCXX_STANDARD 17
    OBJCXX_STANDARD_REQUIRED ON
)

target_include_directories(cheesebridge_host PRIVATE
    src
    ${VULKAN_HEADERS_INCLUDE_DIR}
)

target_compile_options(cheesebridge_host PRIVATE
    -fobjc-arc
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
)

target_link_libraries(cheesebridge_host PRIVATE
    cheesebridge_proto
    "-framework Cocoa"
    "-framework Metal"
    "-framework QuartzCore"
)

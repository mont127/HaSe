# CheeseBridge guest ICD: Linux-only.
#
# Build:
#   mkdir build && cd build
#   cmake ../cheesebridge -DCHEESEBRIDGE_BUILD_GUEST=ON
#   cmake --build .
#
# Install:
#   sudo cmake --install .
# After install the loader will discover the ICD via the manifest at
#   /etc/vulkan/icd.d/cheesebridge_icd.json (or the per-user equivalent).
#
# To run without installing, point the loader at the build tree:
#   export VK_DRIVER_FILES=$PWD/cheesebridge_icd.json   (loader >= 1.3.207)
#   export VK_ICD_FILENAMES=$PWD/cheesebridge_icd.json  (older loaders)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "CheeseBridge guest only builds on Linux. "
        "On macOS build the host (-DCHEESEBRIDGE_BUILD_HOST=ON).")
endif()

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

add_library(cheesebridge_icd SHARED
    src/icd_log.c
    src/icd_handles.c
    src/icd_stub.c
    src/icd_transport.c
    src/icd_loader.c
    src/icd_instance.c
    src/icd_device.c
    src/icd_memory.c
    src/icd_pipeline.c
    src/icd_command.c
    src/icd_sync.c
    src/icd_swapchain.c
    src/icd_queue.c
)

target_include_directories(cheesebridge_icd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)
target_link_libraries(cheesebridge_icd PRIVATE
    cheesebridge_proto
    Threads::Threads
)

if (TARGET Vulkan::Headers)
    target_link_libraries(cheesebridge_icd PRIVATE Vulkan::Headers)
endif()

set_target_properties(cheesebridge_icd PROPERTIES
    OUTPUT_NAME           "CheeseBridge_icd"
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD            11
    C_STANDARD_REQUIRED   ON
    C_VISIBILITY_PRESET   hidden
    VISIBILITY_INLINES_HIDDEN ON
    LINK_FLAGS            "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cheesebridge_icd.ver"
)

target_compile_options(cheesebridge_icd PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
)

# Generate the ICD manifest pointing at the installed library.
set(CB_ICD_INSTALL_PATH "${CMAKE_INSTALL_PREFIX}/lib/libCheeseBridge_icd.so")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cheesebridge_icd.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/cheesebridge_icd.json
    @ONLY
)
# Plus a build-tree manifest that points at the binary directory, useful
# with VK_DRIVER_FILES during development.
set(CB_ICD_INSTALL_PATH "${CMAKE_CURRENT_BINARY_DIR}/libCheeseBridge_icd.so")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cheesebridge_icd.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/cheesebridge_icd.dev.json
    @ONLY
)

include(GNUInstallDirs)
install(TARGETS cheesebridge_icd LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cheesebridge_icd.json
        DESTINATION /etc/vulkan/icd.d)

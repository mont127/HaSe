find_package(Threads REQUIRED)

add_library(cheesebridge_demo_net STATIC
    demo_net.c
)

target_include_directories(cheesebridge_demo_net PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(cheesebridge_demo_net PUBLIC
    cheesebridge_proto
)

set_target_properties(cheesebridge_demo_net PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

add_executable(cheesebridge_fake_host
    fake_host.c
)

target_link_libraries(cheesebridge_fake_host PRIVATE
    cheesebridge_demo_net
    Threads::Threads
)

add_executable(cheesebridge_guest_demo
    guest_demo.c
)

target_link_libraries(cheesebridge_guest_demo PRIVATE
    cheesebridge_demo_net
)

foreach(target cheesebridge_fake_host cheesebridge_guest_demo)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
    )
    if (NOT WIN32)
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wno-unused-parameter
        )
    endif()
endforeach()

# ---- Phase 3 end-to-end clear demo ---------------------------------------
# Links against the system Vulkan loader, then VK_DRIVER_FILES picks up the
# CheeseBridge ICD and routes calls to the macOS host. Only useful on Linux
# (the guest side); skip on macOS where there is no real Vulkan loader.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Vulkan QUIET)
    if (Vulkan_FOUND)
        add_executable(cheesebridge_clear_demo
            cheesebridge_clear_demo.c
        )
        target_link_libraries(cheesebridge_clear_demo PRIVATE
            Vulkan::Vulkan
            ${CMAKE_DL_LIBS}
        )
        set_target_properties(cheesebridge_clear_demo PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED ON
        )
        target_compile_options(cheesebridge_clear_demo PRIVATE
            -Wall -Wextra -Wno-unused-parameter
        )
    else()
        message(STATUS
            "Vulkan headers/loader not found - cheesebridge_clear_demo will not be built")
    endif()
endif()

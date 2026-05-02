add_executable(hasectl
    src/hasectl.c
)

set_target_properties(hasectl PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

if (NOT WIN32)
    target_compile_options(hasectl PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
endif()

if (APPLE)
    enable_language(OBJCXX)

    add_executable(hase_window_host
        src/hase_window_host.mm
    )

    set_target_properties(hase_window_host PROPERTIES
        OBJCXX_STANDARD 17
        OBJCXX_STANDARD_REQUIRED ON
    )

    target_compile_options(hase_window_host PRIVATE
        -fobjc-arc
        -Wall -Wextra -Wno-unused-parameter
    )

    target_link_libraries(hase_window_host PRIVATE
        "-framework Cocoa"
    )
endif()

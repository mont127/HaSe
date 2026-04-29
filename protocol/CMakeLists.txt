# Protocol code is shared by guest and host. Build it once as a static lib
# that both consumers can link against.

add_library(cheesebridge_proto STATIC
    cheesebridge_wire.c
)

target_include_directories(cheesebridge_proto PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(cheesebridge_proto PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

if (NOT WIN32)
    target_compile_options(cheesebridge_proto PRIVATE
        -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden
    )
endif()

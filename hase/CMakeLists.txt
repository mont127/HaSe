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

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

# ---- Compile a GLSL file to a SPIR-V C header ----------------------------
# Generates ${OUTPUT_HEADER} containing `static const uint32_t SYMBOL[] = {...}`.
function(cb_glsl_to_spv_header GLSL_FILE STAGE OUTPUT_HEADER SYMBOL)
    if (NOT GLSLANG_VALIDATOR)
        return()
    endif()
    set(SPV_FILE "${CMAKE_CURRENT_BINARY_DIR}/${SYMBOL}.spv")
    add_custom_command(
        OUTPUT  "${OUTPUT_HEADER}"
        COMMAND ${GLSLANG_VALIDATOR} -V -S ${STAGE} -o "${SPV_FILE}" "${GLSL_FILE}"
        COMMAND ${CMAKE_COMMAND}
                -DSPV_FILE=${SPV_FILE}
                -DHEADER=${OUTPUT_HEADER}
                -DSYMBOL=${SYMBOL}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/spv_to_header.cmake
        DEPENDS "${GLSL_FILE}" "${CMAKE_CURRENT_SOURCE_DIR}/spv_to_header.cmake"
        COMMENT "Compiling ${GLSL_FILE} -> ${OUTPUT_HEADER}"
        VERBATIM
    )
endfunction()

# ---- Phase 3 end-to-end clear demo ---------------------------------------
# Links against the system Vulkan loader, then VK_DRIVER_FILES picks up the
# CheeseBridge ICD and routes calls to the macOS host. Only useful on Linux
# (the guest side); skip on macOS where there is no real Vulkan loader.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Vulkan QUIET)
    find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang)
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

        # ---- Phase 4 triangle demo (needs glslangValidator for shaders) ----
        if (GLSLANG_VALIDATOR)
            set(TRI_VERT_HDR "${CMAKE_CURRENT_BINARY_DIR}/triangle_vert_spv.h")
            set(TRI_FRAG_HDR "${CMAKE_CURRENT_BINARY_DIR}/triangle_frag_spv.h")
            cb_glsl_to_spv_header(
                "${CMAKE_CURRENT_SOURCE_DIR}/triangle.vert.glsl"
                vert "${TRI_VERT_HDR}" triangle_vert_spv)
            cb_glsl_to_spv_header(
                "${CMAKE_CURRENT_SOURCE_DIR}/triangle.frag.glsl"
                frag "${TRI_FRAG_HDR}" triangle_frag_spv)

            add_executable(cheesebridge_triangle_demo
                cheesebridge_triangle_demo.c
                "${TRI_VERT_HDR}"
                "${TRI_FRAG_HDR}"
            )
            target_include_directories(cheesebridge_triangle_demo PRIVATE
                "${CMAKE_CURRENT_BINARY_DIR}"
            )
            target_link_libraries(cheesebridge_triangle_demo PRIVATE
                Vulkan::Vulkan
                ${CMAKE_DL_LIBS}
            )
            set_target_properties(cheesebridge_triangle_demo PROPERTIES
                C_STANDARD 11
                C_STANDARD_REQUIRED ON
            )
            target_compile_options(cheesebridge_triangle_demo PRIVATE
                -Wall -Wextra -Wno-unused-parameter
            )
        else()
            message(STATUS
                "glslangValidator not found - cheesebridge_triangle_demo will not be built")
        endif()
    else()
        message(STATUS
            "Vulkan headers/loader not found - clear/triangle demos will not be built")
    endif()
endif()

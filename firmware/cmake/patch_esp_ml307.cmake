# AI-assisted (GPT-6, OpenAI): compile a patched build copy; retain the managed
# component's original content/hash so IDF dependency validation stays intact.
idf_component_get_property(dotty_tcp_dir 78__esp-ml307 COMPONENT_DIR)
idf_component_get_property(dotty_tcp_lib 78__esp-ml307 COMPONENT_LIB)
set(dotty_tcp_patch "${CMAKE_CURRENT_LIST_DIR}/../patches/esp-ml307-receive-task.patch")
set(dotty_tcp_copy_dir "${CMAKE_BINARY_DIR}/dotty-esp-ml307")
set(dotty_tcp_source "${dotty_tcp_dir}/src/esp/esp_tcp.cc")
set(dotty_tcp_copy "${dotty_tcp_copy_dir}/src/esp/esp_tcp.cc")
find_program(DOTTY_PATCH_EXECUTABLE patch REQUIRED)
file(MAKE_DIRECTORY "${dotty_tcp_copy_dir}/src/esp")
configure_file("${dotty_tcp_source}" "${dotty_tcp_copy}" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${dotty_tcp_patch}")
execute_process(
    COMMAND "${DOTTY_PATCH_EXECUTABLE}" --batch --fuzz=0 -p1 --input "${dotty_tcp_patch}"
    WORKING_DIRECTORY "${dotty_tcp_copy_dir}"
    RESULT_VARIABLE dotty_tcp_patch_result
    OUTPUT_VARIABLE dotty_tcp_patch_output
    ERROR_VARIABLE dotty_tcp_patch_error)
if(NOT dotty_tcp_patch_result EQUAL 0)
    message(FATAL_ERROR "Dotty TCP patch does not apply: ${dotty_tcp_patch_output}${dotty_tcp_patch_error}")
endif()
get_target_property(dotty_tcp_sources "${dotty_tcp_lib}" SOURCES)
set(dotty_tcp_matches 0)
set(dotty_tcp_remaining "")
foreach(dotty_source IN LISTS dotty_tcp_sources)
    if(dotty_source MATCHES "(^|/)src/esp/esp_tcp\\.cc$")
        math(EXPR dotty_tcp_matches "${dotty_tcp_matches} + 1")
    else()
        list(APPEND dotty_tcp_remaining "${dotty_source}")
    endif()
endforeach()
if(NOT dotty_tcp_matches EQUAL 1)
    message(FATAL_ERROR "Expected exactly one EspTcp source, found ${dotty_tcp_matches}")
endif()
set_property(TARGET "${dotty_tcp_lib}" PROPERTY SOURCES "${dotty_tcp_remaining};${dotty_tcp_copy}")
target_include_directories("${dotty_tcp_lib}" PRIVATE "${dotty_tcp_dir}/src/esp")

# Dotty (#200), added by Claude Opus 5.5 (Anthropic): WebSocket teardown order.
# Same copy-and-patch step for src/web_socket.cc.
set(dotty_ws_patch "${CMAKE_CURRENT_LIST_DIR}/../patches/esp-ml307-websocket-teardown.patch")
set(dotty_ws_copy "${dotty_tcp_copy_dir}/src/web_socket.cc")
configure_file("${dotty_tcp_dir}/src/web_socket.cc" "${dotty_ws_copy}" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${dotty_ws_patch}")
execute_process(
    COMMAND "${DOTTY_PATCH_EXECUTABLE}" --batch --fuzz=0 -p1 --input "${dotty_ws_patch}"
    WORKING_DIRECTORY "${dotty_tcp_copy_dir}"
    RESULT_VARIABLE dotty_ws_patch_result
    OUTPUT_VARIABLE dotty_ws_patch_output
    ERROR_VARIABLE dotty_ws_patch_error)
if(NOT dotty_ws_patch_result EQUAL 0)
    message(FATAL_ERROR "Dotty WebSocket patch does not apply: ${dotty_ws_patch_output}${dotty_ws_patch_error}")
endif()
get_target_property(dotty_ws_sources "${dotty_tcp_lib}" SOURCES)
set(dotty_ws_matches 0)
set(dotty_ws_remaining "")
foreach(dotty_source IN LISTS dotty_ws_sources)
    if(dotty_source MATCHES "(^|/)src/web_socket\\.cc$" AND NOT dotty_source STREQUAL dotty_ws_copy)
        math(EXPR dotty_ws_matches "${dotty_ws_matches} + 1")
    else()
        list(APPEND dotty_ws_remaining "${dotty_source}")
    endif()
endforeach()
if(NOT dotty_ws_matches EQUAL 1)
    message(FATAL_ERROR "Expected exactly one WebSocket source, found ${dotty_ws_matches}")
endif()
set_property(TARGET "${dotty_tcp_lib}" PROPERTY SOURCES "${dotty_ws_remaining};${dotty_ws_copy}")

# Dotty (#189), added by Claude Opus 5.5 (Anthropic): bound esp_video's
# VIDIOC_DQBUF wait. Same copy-and-patch approach as patch_esp_ml307.cmake: the
# managed component keeps its original content/hash; only a build copy of
# src/esp_video_ioctl.c is patched. No extra task or thread is needed.
idf_component_get_property(dotty_vid_dir espressif__esp_video COMPONENT_DIR)
idf_component_get_property(dotty_vid_lib espressif__esp_video COMPONENT_LIB)
set(dotty_vid_patch "${CMAKE_CURRENT_LIST_DIR}/../patches/esp-video-dqbuf-timeout.patch")
set(dotty_vid_copy_dir "${CMAKE_BINARY_DIR}/dotty-esp-video")
set(dotty_vid_copy "${dotty_vid_copy_dir}/src/esp_video_ioctl.c")
find_program(DOTTY_PATCH_EXECUTABLE patch REQUIRED)
file(MAKE_DIRECTORY "${dotty_vid_copy_dir}/src")
configure_file("${dotty_vid_dir}/src/esp_video_ioctl.c" "${dotty_vid_copy}" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${dotty_vid_patch}")
execute_process(
    COMMAND "${DOTTY_PATCH_EXECUTABLE}" --batch --fuzz=0 -p1 --input "${dotty_vid_patch}"
    WORKING_DIRECTORY "${dotty_vid_copy_dir}"
    RESULT_VARIABLE dotty_vid_patch_result
    OUTPUT_VARIABLE dotty_vid_patch_output
    ERROR_VARIABLE dotty_vid_patch_error)
if(NOT dotty_vid_patch_result EQUAL 0)
    message(FATAL_ERROR "Dotty esp_video patch does not apply: ${dotty_vid_patch_output}${dotty_vid_patch_error}")
endif()
get_target_property(dotty_vid_sources "${dotty_vid_lib}" SOURCES)
set(dotty_vid_matches 0)
set(dotty_vid_remaining "")
foreach(dotty_source IN LISTS dotty_vid_sources)
    if(dotty_source MATCHES "(^|/)src/esp_video_ioctl\\.c$")
        math(EXPR dotty_vid_matches "${dotty_vid_matches} + 1")
    else()
        list(APPEND dotty_vid_remaining "${dotty_source}")
    endif()
endforeach()
if(NOT dotty_vid_matches EQUAL 1)
    message(FATAL_ERROR "Expected exactly one esp_video_ioctl.c source, found ${dotty_vid_matches}")
endif()
set_property(TARGET "${dotty_vid_lib}" PROPERTY SOURCES "${dotty_vid_remaining};${dotty_vid_copy}")
target_include_directories("${dotty_vid_lib}" PRIVATE "${dotty_vid_dir}/src" "${dotty_vid_dir}/private_include")

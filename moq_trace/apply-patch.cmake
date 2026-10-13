# Apply PATCH to the source tree in the working directory, once.
#
# FetchContent reruns the patch step whenever the dependency's details change,
# so a patch that is already applied is left as it is rather than failing.
execute_process(
    COMMAND git apply --reverse --check "${PATCH}"
    RESULT_VARIABLE already_applied
    OUTPUT_QUIET ERROR_QUIET)
if(already_applied EQUAL 0)
    return()
endif()
execute_process(COMMAND git apply "${PATCH}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "failed to apply ${PATCH}")
endif()

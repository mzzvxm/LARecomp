file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(header "${OUTPUT_DIR}/race_line_twin_gen.h")
file(REMOVE "${header}")

if(PYTHON)
    execute_process(
        COMMAND "${PYTHON}" -I "${GENERATOR}" "${INPUT_DIR}" "${header}"
        RESULT_VARIABLE result
        OUTPUT_QUIET
        ERROR_VARIABLE diagnostic)
    if(NOT result STREQUAL "0")
        string(STRIP "${diagnostic}" reason)
        if(NOT reason)
            set(reason "generator result: ${result}")
        endif()
        string(REGEX REPLACE "[\r\n]+" " " reason "${reason}")
    elseif(NOT EXISTS "${header}")
        set(reason "generator produced no header")
    endif()
else()
    set(reason "Python 3.10 or newer was not found")
endif()

if(DEFINED reason)
    file(REMOVE "${header}")
    message(STATUS "Race Line twins disabled for this build (${reason})")
endif()
# The stamp is the required output. An absent header leaves the recompiled path active.
file(TOUCH "${OUTPUT_DIR}/race_line_twin.stamp")

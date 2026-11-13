# larecomp_download_with_bar(<url> <file> <size> <label> <status_var>)
#
# Downloads <url> to <file> with one progress bar that redraws in place, where file(DOWNLOAD
# SHOW_PROGRESS) prints a line for every percent. <size> is the file's size in bytes, pinned next to its
# SHA-256 (bump both together); the caller checks the hash. The file comes in pieces through HTTP range
# requests (file(DOWNLOAD RANGE_START/RANGE_END), CMake 3.24), the bar is drawn again after each piece,
# and the pieces are joined at the end (for a moment the pieces and the joined file are both on disk).
# Each piece is tried 3 times. A server that ignores the range and sends the whole file is fine: that
# first piece is the file.
#
# <status_var> is set like file(DOWNLOAD ... STATUS): 0 and "No error", or a code and a message.
#
# In a terminal the bar is one line (a carriage return before each redraw). An IDE's output window
# that does not honour carriage returns shows one line per piece instead (34 for the 276 MB Streamline zip).
# Each piece is a request of its own: about 9 s more than one request for that zip at 30 MB/s, once.

set(_larecomp_dl_piece_bytes 8388608)  # 8 MiB

function(_larecomp_dl_bar label done total)
    math(EXPR _percent "${done} * 100 / ${total}")
    math(EXPR _filled "${done} * 30 / ${total}")
    math(EXPR _empty "30 - ${_filled}")
    math(EXPR _mb_done "${done} / 1000000")
    math(EXPR _mb_total "${total} / 1000000")
    string(REPEAT "#" ${_filled} _bar_done)
    string(REPEAT "-" ${_empty} _bar_left)
    set(_pad "")
    if(_percent LESS 10)
        set(_pad "  ")
    elseif(_percent LESS 100)
        set(_pad " ")
    endif()
    # The line only grows (the numbers only go up), so each redraw covers the one before.
    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo_append
        "\r-- ${label} [${_bar_done}${_bar_left}] ${_pad}${_percent}% (${_mb_done} of ${_mb_total} MB)")
endfunction()

function(larecomp_download_with_bar url file size label status_var)
    set(_dir "${file}.pieces")
    file(REMOVE_RECURSE "${_dir}")
    file(MAKE_DIRECTORY "${_dir}")
    math(EXPR _last "(${size} + ${_larecomp_dl_piece_bytes} - 1) / ${_larecomp_dl_piece_bytes} - 1")
    set(_pieces "")
    set(_done 0)
    set(_whole FALSE)
    set(_failed "")
    _larecomp_dl_bar("${label}" 0 ${size})
    foreach(_i RANGE ${_last})
        math(EXPR _start "${_i} * ${_larecomp_dl_piece_bytes}")
        math(EXPR _end "${_start} + ${_larecomp_dl_piece_bytes} - 1")  # inclusive, like HTTP's Range
        if(_end GREATER_EQUAL size)
            math(EXPR _end "${size} - 1")
        endif()
        math(EXPR _want "${_end} - ${_start} + 1")
        set(_piece "${_dir}/${_i}")
        set(_got FALSE)
        foreach(_try 1 2 3)
            file(DOWNLOAD "${url}" "${_piece}" RANGE_START ${_start} RANGE_END ${_end}
                 INACTIVITY_TIMEOUT 120 STATUS _status TLS_VERIFY ON)
            list(GET _status 0 _code)
            set(_have -1)
            if(_code EQUAL 0 AND EXISTS "${_piece}")
                file(SIZE "${_piece}" _have)
            endif()
            if(_code EQUAL 0 AND _have EQUAL _want)
                set(_got TRUE)
                break()
            endif()
            if(_code EQUAL 0 AND _i EQUAL 0 AND _have EQUAL size)
                set(_whole TRUE)  # the range was ignored: this is the whole file
                break()
            endif()
            if(_code EQUAL 0)
                set(_status "1;\"bytes ${_start}-${_end}: got ${_have} bytes, expected ${_want}\"")
            endif()
            file(REMOVE "${_piece}")
        endforeach()
        if(_whole)
            file(RENAME "${_piece}" "${file}")
            _larecomp_dl_bar("${label}" ${size} ${size})
            break()
        endif()
        if(NOT _got)
            set(_failed "${_status}")
            break()
        endif()
        list(APPEND _pieces "${_piece}")
        math(EXPR _done "${_done} + ${_want}")
        _larecomp_dl_bar("${label}" ${_done} ${size})
    endforeach()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "")  # ends the bar's line

    if(NOT _failed AND NOT _whole)
        file(REMOVE "${file}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E cat ${_pieces} OUTPUT_FILE "${file}"
                        RESULT_VARIABLE _rc)
        set(_have -1)
        if(EXISTS "${file}")
            file(SIZE "${file}" _have)
        endif()
        if(NOT _rc EQUAL 0 OR NOT _have EQUAL size)
            file(REMOVE "${file}")
            set(_failed "1;\"joining the pieces failed (${_rc}; ${_have} of ${size} bytes)\"")
        endif()
    endif()
    file(REMOVE_RECURSE "${_dir}")
    if(_failed)
        set(${status_var} "${_failed}" PARENT_SCOPE)
    else()
        set(${status_var} "0;\"No error\"" PARENT_SCOPE)
    endif()
endfunction()

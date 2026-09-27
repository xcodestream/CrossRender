# Сжимает результаты WASM-сборки в сайдкары .gz/.br. Вызывается как POST_BUILD
# шаг примера (см. CMake-флаги CR_WASM_GZIP / CR_WASM_BROTLI). Оригинальные
# файлы сохраняются: раздача предсжатых копий - забота tools/serve_wasm.py.
set(_files crossrender_example.js crossrender_example.wasm crossrender_example.data crossrender_example.html)

foreach(_name ${_files})
    set(_f "${OUTDIR}/${_name}")
    if(NOT EXISTS "${_f}")
        continue()
    endif()

    if(GZIP_EXE AND EXISTS "${GZIP_EXE}")
        execute_process(COMMAND "${GZIP_EXE}" -9 -k -f "${_f}" RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(WARNING "wasm_compress: gzip failed for ${_name}")
        endif()
    endif()

    if(BROTLI_EXE AND EXISTS "${BROTLI_EXE}")
        execute_process(COMMAND "${BROTLI_EXE}" -q 11 -k -f "${_f}" RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(WARNING "wasm_compress: brotli failed for ${_name}")
        endif()
    elseif(BROTLI_PYTHON)
        execute_process(COMMAND "${BROTLI_PYTHON}" -c
            "import brotli,sys; open(sys.argv[1]+'.br','wb').write(brotli.compress(open(sys.argv[1],'rb').read(), quality=11))"
            "${_f}" RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(WARNING "wasm_compress: python-brotli failed for ${_name}")
        endif()
    endif()
endforeach()

# Итоговый отчёт: на глаз видно, что реально уйдёт по сети.
foreach(_name ${_files})
    set(_f "${OUTDIR}/${_name}")
    if(NOT EXISTS "${_f}")
        continue()
    endif()
    file(SIZE "${_f}" _size)
    set(_report "${_name}: ${_size}")
    if(EXISTS "${_f}.gz")
        file(SIZE "${_f}.gz" _gz)
        string(APPEND _report ", gz ${_gz}")
    endif()
    if(EXISTS "${_f}.br")
        file(SIZE "${_f}.br" _br)
        string(APPEND _report ", br ${_br}")
    endif()
    message(STATUS "wasm_compress: ${_report} bytes")
endforeach()

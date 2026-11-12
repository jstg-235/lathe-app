# The bundled MP3 encoder: LAME's own `lame` command-line program, built from its release source
# (LGPL). Lathe runs it as a separate process to make MP3s; it isn't linked into the app.
# Set LATHE_LAME_SOURCE to an unpacked LAME source folder (CI downloads the official 3.100 release
# and checks its SHA-512). Without it, MP3 export says the encoder is missing.
set(LATHE_LAME_SOURCE "" CACHE PATH "Unpacked LAME source (for MP3 export)")
if(LATHE_LAME_SOURCE AND EXISTS "${LATHE_LAME_SOURCE}/libmp3lame/lame.c")
    file(GLOB LATHE_LAME_LIB "${LATHE_LAME_SOURCE}/libmp3lame/*.c")
    set(LATHE_LAME_FRONTEND
        "${LATHE_LAME_SOURCE}/frontend/main.c" "${LATHE_LAME_SOURCE}/frontend/lame_main.c" "${LATHE_LAME_SOURCE}/frontend/get_audio.c"
        "${LATHE_LAME_SOURCE}/frontend/parse.c" "${LATHE_LAME_SOURCE}/frontend/timestatus.c" "${LATHE_LAME_SOURCE}/frontend/brhist.c"
        "${LATHE_LAME_SOURCE}/frontend/console.c" "${LATHE_LAME_SOURCE}/frontend/lametime.c")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/lame-config.h" "${CMAKE_BINARY_DIR}/lame-config/config.h" COPYONLY)
    add_executable(lame ${LATHE_LAME_LIB} ${LATHE_LAME_FRONTEND})
    target_include_directories(lame PRIVATE "${CMAKE_BINARY_DIR}/lame-config" "${LATHE_LAME_SOURCE}/include" "${LATHE_LAME_SOURCE}/libmp3lame"
        "${LATHE_LAME_SOURCE}/frontend" "${LATHE_LAME_SOURCE}/mpglib")
    target_compile_definitions(lame PRIVATE HAVE_CONFIG_H)
    # (LAME is older C: quiet its warnings, and keep implicit declarations legal on new compilers)
    target_compile_options(lame PRIVATE -w -Wno-implicit-function-declaration -Wno-error=implicit-function-declaration -Wno-implicit-int)
    if(NOT APPLE)
        target_link_libraries(lame PRIVATE m)
    endif()
    set(LATHE_HAVE_LAME ON)
    message(STATUS "LAME MP3 encoder: building from ${LATHE_LAME_SOURCE}")
else()
    set(LATHE_HAVE_LAME OFF)
    message(STATUS "LAME MP3 encoder: not built (set LATHE_LAME_SOURCE)")
endif()

# Copies SRC to DST only when DST does not exist yet. Used to seed
# stocktool.cfg next to the built exe without clobbering a config the user
# has since edited through the app.
if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "copy_if_missing.cmake needs -DSRC=<file> -DDST=<file>")
endif()
if(NOT EXISTS "${DST}")
    file(COPY_FILE "${SRC}" "${DST}")
    message(STATUS "Seeded ${DST}")
endif()

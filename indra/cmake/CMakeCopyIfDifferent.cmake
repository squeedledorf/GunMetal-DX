# -*- cmake -*-
# Taken from http://www.cmake.org/Wiki/CMakeCopyIfDifferent
# Generates a rule to copy each source file from source directory to destination directory.
#
# Typical use -
#
# SET(SRC_FILES head1.h head2.h head3.h)
# COPY_IF_DIFFERENT( /from_dir /to_dir IncludeTargets ${SRC_FILES})
# ADD_TARGET(CopyIncludes ALL DEPENDS ${IncludeTargets})

MACRO(COPY_IF_DIFFERENT FROM_DIR TO_DIR TARGETS)
# Macro to implement copy_if_different for a list of files
# Arguments -
#   FROM_DIR - this is the source directory
#   TO_DIR   - this is the destination directory
#   TARGETS  - A variable to receive a list of targets
#   FILES    - names of the files to copy
#              TODO: add globing.
SET(AddTargets "")
FOREACH(SRC ${ARGN})
    GET_FILENAME_COMPONENT(SRCFILE ${SRC} NAME)
    IF("${FROM_DIR}" STREQUAL "")
        SET(FROM ${SRC})
    ELSE("${FROM_DIR}" STREQUAL "")
        SET(FROM ${FROM_DIR}/${SRC})
    ENDIF("${FROM_DIR}" STREQUAL "")
    IF("${TO_DIR}" STREQUAL "")
        SET(TO ${SRCFILE})
    ELSE("${TO_DIR}" STREQUAL "")
        SET(TO   ${TO_DIR}/${SRCFILE})
    ENDIF("${TO_DIR}" STREQUAL "")
    # The symbolic ".stage" output makes this rule run on every build, so the content
    # comparison in copy_if_different always happens. A plain mtime dependency on FROM
    # misses package updates whose files carry an OLDER mtime than the staged copy:
    # #647 shipped OpenJPEG 2.4.0's openjp2.dll beside an exe built against 2.5.4.
    # copy_if_different leaves an identical TO untouched, and custom commands restat,
    # so nothing downstream rebuilds when nothing changed.
    SET_SOURCE_FILES_PROPERTIES("${TO}.stage" PROPERTIES SYMBOLIC TRUE)
    ADD_CUSTOM_COMMAND(
        OUTPUT  "${TO}" "${TO}.stage"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FROM} ${TO}
        DEPENDS ${FROM}
        )
    SET(AddTargets ${AddTargets} ${TO})
ENDFOREACH(SRC ${ARGN})
SET(${TARGETS} ${AddTargets})
ENDMACRO(COPY_IF_DIFFERENT FROM_DIR TO_DIR TARGETS)

# -*- cmake -*-
# Construct the version and copyright information based on package data
# (packages-info.txt for the About box) from the 3p bookkeeping Prebuilt.cmake keeps.
include(Python)
include(Prebuilt)

include_guard()

add_custom_command(OUTPUT packages-info.txt
  COMMENT "Generating packages-info.txt for the about box"
  MAIN_DEPENDENCY ${SL_3P_MANIFEST}
  DEPENDS ${CMAKE_SOURCE_DIR}/../scripts/packages-formatter.py
          ${CMAKE_SOURCE_DIR}/../scripts/3p/llsd_xml.py
          ${SL_3P_MANIFEST}
  COMMAND ${PYTHON_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/../scripts/packages-formatter.py
          --manifest ${SL_3P_MANIFEST}
          "${VIEWER_CHANNEL}" "${VIEWER_SHORT_VERSION}.${VIEWER_VERSION_REVISION}" "${LIBS_PREBUILT_DIR}" > packages-info.txt
  )

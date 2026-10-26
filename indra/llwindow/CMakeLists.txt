# -*- cmake -*-

project(llwindow)

include(00-Common)
include(DragDrop)
include(Linking)
include(LLCommon)
include(LLImage)
include(LLWindow)
include(UI)
include(ViewerMiscLibs)
include(GLM)
include(SDL3)

set(llwindow_SOURCE_FILES
    llcursortypes.cpp
    llgamecontrol.cpp
    llgamecontroltranslator.cpp
    llkeyboard.cpp
    llwindowcallbacks.cpp
    llwindow.cpp
    )

set(llwindow_HEADER_FILES
    CMakeLists.txt

    llcursortypes.h
    llgamecontrol.h
    llgamecontroltranslator.h
    llkeyboard.h
    llwindowcallbacks.h
    )

set(viewer_SOURCE_FILES
    llmousehandler.cpp
    )

set(viewer_HEADER_FILES
    llwindow.h
    llpreeditor.h
    llmousehandler.h
    )

set(llwindow_LINK_LIBRARIES
        llcommon
        llimage
        llmath
        llrender
        llfilesystem
        llxml
        ll::glm
        ll::uilibraries
        ll::SDL3
        )

list(APPEND viewer_SOURCE_FILES
     llsdl.cpp
     llkeyboardsdl.cpp
     llwindowsdl.cpp
     )
list(APPEND viewer_HEADER_FILES
     llsdl.h
     llkeyboardsdl.h
     llwindowsdl.h
     )

list(APPEND llwindow_LINK_LIBRARIES
     ll::SDL3
     )

if (WINDOWS)
  list(APPEND llwindow_SOURCE_FILES
       lldxhardware.cpp
       )
  list(APPEND llwindow_HEADER_FILES
       lldxhardware.h
       )
  list(APPEND llwindow_LINK_LIBRARIES
       wbemuuid
       comdlg32     # Common Dialogs for ChooseColor
       ole32
       dxgi
       d3d9
       dinput8      # #847: joystick / 3D mouse (libndofdev) via DirectInput8
       dxguid
       )
endif ()

if (llwindow_HEADER_FILES)
  list(APPEND llwindow_SOURCE_FILES ${llwindow_HEADER_FILES})
endif (llwindow_HEADER_FILES)

  list(APPEND viewer_SOURCE_FILES ${viewer_HEADER_FILES})

  add_library (llwindow
    ${llwindow_SOURCE_FILES}
    ${viewer_SOURCE_FILES}
    )

target_link_libraries (llwindow ${llwindow_LINK_LIBRARIES})
target_include_directories(llwindow INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions( llwindow PUBLIC LL_SDL_WINDOW=1 )

if (USE_PRECOMPILED_HEADERS)
  target_precompile_headers(llwindow REUSE_FROM llprecompiled)
endif ()


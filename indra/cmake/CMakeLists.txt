# -*- cmake -*-

include(00-Common)

project(cmake)

set(cmake_SOURCE_FILES
        CMakeLists.txt
        00-Common.cmake
        APR.cmake
        Audio.cmake
        Boost.cmake
        Brotli.cmake
        BuildPackagesInfo.cmake
        BuildVersion.cmake
        CEFPlugin.cmake
        CEFPlugin.cmake
        CMakeCopyIfDifferent.cmake
        Copy3rdPartyLibs.cmake
        CURL.cmake
        DeploySharedLibs.cmake
        DragDrop.cmake
        EXPAT.cmake
        3p_manifest.json
        FindPipeWire.cmake
        FreeType.cmake
        GLIB.cmake
        GLM.cmake
        GStreamer10Plugin.cmake
        Hunspell.cmake
        LLAddBuildTest.cmake
        LLAppearance.cmake
        LLAudio.cmake
        LLBuildFlags.cmake
        LLCommon.cmake
        LLImage.cmake
        LLPhysicsExtensions.cmake
        LLPrimitive.cmake
        LLSharedLibs.cmake
        LLTestCommand.cmake
        LLWindow.cmake
        Linking.cmake
        NDOF.cmake
        OPENAL.cmake
        OpenJPEG.cmake
        OpenSSL.cmake
        PNG.cmake
        PluginAPI.cmake
        Prebuilt.cmake
        PulseAudio.cmake
        Python.cmake
        SDL3.cmake
        SSE2NEON.cmake
        TemplateCheck.cmake
        TinyGLTF.cmake
        Tracy.cmake
        Tut.cmake
        UI.cmake
        UnixInstall.cmake
        Variables.cmake
        VHACD.cmake
        ViewerMiscLibs.cmake
        VisualLeakDetector.cmake
        LibVLCPlugin.cmake
        WebRTC.cmake
        xxHash.cmake
        ZLIBNG.cmake
        )

source_group("Shared Rules" FILES ${cmake_SOURCE_FILES})

set(master_SOURCE_FILES
        ../CMakeLists.txt
        )

source_group("Master Rules" FILES ${master_SOURCE_FILES})

add_library(cmake
        cmake_dummy.cpp
        ${cmake_SOURCE_FILES}
        ${master_SOURCE_FILES}
        )

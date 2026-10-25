# -*- cmake -*-

# some webrtc headers require C++ 20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(00-Common)
include(Linking)
include(WebRTC)

project(llwebrtc)

set(llwebrtc_SOURCE_FILES
    llwebrtc.cpp
    )

set(llwebrtc_HEADER_FILES
    CMakeLists.txt
    llwebrtc.h
    llwebrtc_impl.h
    )

list(APPEND llwebrtc_SOURCE_FILES ${llwebrtc_HEADER_FILES})

add_library (llwebrtc SHARED ${llwebrtc_SOURCE_FILES})

set_target_properties(llwebrtc PROPERTIES PUBLIC_HEADER llwebrtc.h)

if (WINDOWS)
    cmake_policy(SET CMP0091 NEW)
    set_target_properties(llwebrtc
        PROPERTIES
        LINK_FLAGS "/debug /LARGEADDRESSAWARE"
        # Emit the DLL straight into the shared-lib staging dir: newview's copy
        # step lists ${SHARED_LIB_STAGING_DIR}/llwebrtc.dll as an input, which a
        # single-config generator (Ninja, the Linux cross build) must see produced.
        RUNTIME_OUTPUT_DIRECTORY "${SHARED_LIB_STAGING_DIR}"
        )
    target_link_libraries(llwebrtc PRIVATE ll::webrtc
                                       secur32
                                       winmm
                                       dmoguids
                                       wmcodecdspuuid
                                       msdmo
                                       strmiids
                                       iphlpapi
                                       crypt32
                                       libcmt)
    # as the webrtc libraries are release, build this binary as release as well.
    target_compile_options(llwebrtc PRIVATE "/MT" "/Zc:wchar_t")
elseif (DARWIN)
    # m144's macOS audio device / SSL code needs these frameworks (as in LL's build).
    target_link_libraries(llwebrtc PRIVATE ll::webrtc
                                   "-framework AVFoundation"
                                   "-framework AudioToolbox"
                                   "-framework CoreAudio"
                                   "-framework CoreMedia"
                                   "-framework CoreVideo"
                                   "-framework Foundation"
                                   "-framework Security"
    )
elseif (LINUX)
    target_link_libraries(llwebrtc PRIVATE ll::webrtc)
    # Keep everything pulled in from libwebrtc.a (BoringSSL, abseil, libyuv, opus,
    # RTC_EXPORT'd webrtc templates) local to this .so: only the llwebrtc:: API is
    # exported, so nothing can interpose with the viewer's OpenSSL & co.
    target_link_options(llwebrtc PRIVATE "LINKER:--exclude-libs,ALL")
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        # Prebuilt webrtc objects bind libm to the newest glibc symbol versions;
        # route them to the GLIBC_2.2.5 versions (llwebrtc_glibc_compat.cpp).
        target_sources(llwebrtc PRIVATE llwebrtc_glibc_compat.cpp)
        target_link_options(llwebrtc PRIVATE "LINKER:--wrap=sqrtf" "LINKER:--wrap=log10f")
    endif ()
endif (WINDOWS)

target_include_directories( llwebrtc INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

if (WINDOWS)
    set_property(TARGET llwebrtc PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreadedDebug")
else()
    target_compile_options(llwebrtc PRIVATE -Wno-deprecated-declarations) # webrtc::CreateAudioDeviceWithDataObserver is deprecated
endif (WINDOWS)

ADD_CUSTOM_COMMAND(TARGET llwebrtc POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different
                       $<TARGET_FILE:llwebrtc>
                               ${SHARED_LIB_STAGING_DIR})
# Add tests
if (LL_TESTS)
endif (LL_TESTS)

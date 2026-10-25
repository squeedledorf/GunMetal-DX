
project(externals)

include(Variables)
include(00-Common)
include(FetchContent)
include(UI)

set(CMAKE_FOLDER "Third Party")
set(CMAKE_POLICY_DEFAULT_CMP0063 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

set(externals_SOURCE_FILES
    CMakeLists.txt
    )

set_source_files_properties(${externals_SOURCE_FILES}
                            PROPERTIES HEADER_FILE_ONLY TRUE)

add_library(externals
            ../cmake/cmake_dummy.cpp
            ${externals_SOURCE_FILES}
            )

# FetchContent_Declare(
#   fmt
#   GIT_REPOSITORY https://github.com/fmtlib/fmt.git
#   GIT_TAG        11.1.0
#   GIT_SHALLOW TRUE
#   GIT_PROGRESS TRUE
#   )

# # #Download the rest of the libraries
# FetchContent_MakeAvailable(fmt)

FetchContent_Declare(
  meshoptimizer
  GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
  GIT_TAG        v0.25
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE
  )

set(MESHOPT_BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(meshoptimizer)

unset(CMAKE_FOLDER)

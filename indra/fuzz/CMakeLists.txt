# -*- cmake -*-
#
# libFuzzer harnesses for the untrusted-input parsers. Only added when -DLL_FUZZ=ON (see
# indra/CMakeLists.txt and indra/cmake/LLFuzz.cmake); the sanitizer flags for the libraries come
# from there, this file adds the per-harness libFuzzer link.
#
#   cmake -S indra --preset linux-fuzz && ninja -C build-linux-fuzz fuzz_all
#   scripts/fuzz/run.sh <target> [seconds]
#
# One .cpp per parser. Each harness links only the libraries it needs, never the viewer.
# See README.md for what is covered and what is not.

project(fuzz)

include(00-Common)
include(LLCommon)
include(LLImage)
include(JPEG)
include(ZLIBNG)
include(TinyGLTF)

add_custom_target(fuzz_all COMMENT "All libFuzzer harnesses")

# ll_add_fuzzer(<name> <source> [LIBS ...] [DEFS ...] [INCLUDES ...])
function(ll_add_fuzzer name source)
  cmake_parse_arguments(F "" "" "LIBS;DEFS;INCLUDES" ${ARGN})
  add_executable(${name} ${source} fuzz_common.h)
  target_link_libraries(${name} PRIVATE ${F_LIBS})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${F_INCLUDES})
  if (F_DEFS)
    target_compile_definitions(${name} PRIVATE ${F_DEFS})
  endif ()
  # The libFuzzer driver (main) and its coverage runtime; the libraries already carry
  # -fsanitize=fuzzer-no-link from LLFuzz.cmake.
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  set_target_properties(${name} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fuzz"
      FOLDER "Fuzz")
  add_dependencies(fuzz_all ${name})
endfunction()

# ---- LLSD (llcommon): HTTP / UDP / cache payloads all go through these ----------------------------
ll_add_fuzzer(fuzz_llsd_xml      fuzz_llsd_xml.cpp      LIBS llcommon)
ll_add_fuzzer(fuzz_llsd_notation fuzz_llsd_notation.cpp LIBS llcommon)
ll_add_fuzzer(fuzz_llsd_binary   fuzz_llsd_binary.cpp   LIBS llcommon)
ll_add_fuzzer(fuzz_llsd_auto     fuzz_llsd_auto.cpp     LIBS llcommon)
ll_add_fuzzer(fuzz_unzip_llsd    fuzz_unzip_llsd.cpp    LIBS llcommon)

# ---- UDP message system (llmessage): zero-code expand + template reader on a raw datagram ----------
get_filename_component(FUZZ_MESSAGE_TEMPLATE_PATH
    "${CMAKE_SOURCE_DIR}/../scripts/messages/message_template.msg" ABSOLUTE)
ll_add_fuzzer(fuzz_message fuzz_message.cpp
    LIBS llmessage llcorehttp llxml llmath llcommon
    DEFS "FUZZ_MESSAGE_TEMPLATE=\"${FUZZ_MESSAGE_TEMPLATE_PATH}\"")

# ---- Mesh assets (llmath / llprimitive) --------------------------------------------------------------
ll_add_fuzzer(fuzz_mesh_lod   fuzz_mesh_lod.cpp   LIBS llmath llcommon)
ll_add_fuzzer(fuzz_mesh_asset fuzz_mesh_asset.cpp LIBS llprimitive llmath llcommon)
ll_add_fuzzer(fuzz_mesh_skin  fuzz_mesh_skin.cpp  LIBS llprimitive llmath llcommon)

# ---- Images (llimage): one binary per codec from fuzz_image.cpp ---------------------------------------
# DXT is left out on purpose: LLImageDXT::updateData() / getMipOffset() raise LL_ERRS on a short
# or old-format header, so every other input would "crash" until those fail soft; it only reads
# local .dds files, never fetched textures. The harness supports IMG_CODEC_DXT when that is fixed.
foreach (codec TGA PNG BMP JPEG TIFF J2C)
  string(TOLOWER ${codec} codec_lower)
  ll_add_fuzzer(fuzz_image_${codec_lower} fuzz_image.cpp
      LIBS llimage llmath llcommon
      DEFS FUZZ_CODEC=IMG_CODEC_${codec})
endforeach ()

# ---- glTF material (llprimitive): fromJSON + the LLSD override that arrives in object updates ------
ll_add_fuzzer(fuzz_gltf_material fuzz_gltf_material.cpp
    LIBS llprimitive llmath llcommon
    INCLUDES "${LIBS_PREBUILT_DIR}/include")

# ---- Inventory / notecard text formats (llinventory) -------------------------------------------------
ll_add_fuzzer(fuzz_notecard  fuzz_notecard.cpp  LIBS llinventory llmath llcommon)
ll_add_fuzzer(fuzz_inventory fuzz_inventory.cpp LIBS llinventory llmath llcommon)

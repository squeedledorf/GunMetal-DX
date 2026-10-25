# -*- cmake -*-
#
# Third-party (3p) prebuilt packages, installed by CMake itself.
#
# use_prebuilt_binary(<name>) installs package <name> from cmake/3p_manifest.json
# (hand-edited; maintain it with scripts/3p/manifest.py) into
# ${PREBUILT_INSTALL_DIR} (<build>/packages), in the layout the old autobuild used
# (include/, lib/release/, LICENSES/, ...), so the Find*/use modules are unchanged:
#
#   1. pick the archive for the target platform (windows64 / darwin64 / linux64,
#      else "common") -- the target, not the host, so the Windows cross build gets
#      the windows64 packages;
#   2. find it in the download cache (SL_3P_CACHE; cache variable, else $SL_3P_CACHE,
#      else ~/.cache/slipstream-3p), accepting <basename> and the old
#      fetch_3p.py <name>-<basename> file names, and also reading the checkout's
#      autobuild-era .autobuild-installables[-win] dirs), or download it there;
#   3. verify it (SHA256 from the manifest, else its recorded hash: natively,
#      or through Python's hashlib for blake2b);
#   4. unpack it, refusing files another package already installed (as autobuild
#      did), keep its autobuild-package.xml metadata (version, copyright; read by
#      scripts/packages-formatter.py for the About box) and its file list, and
#      record the installed hash.
#
# A package is reinstalled only when its hash in the manifest changes (the old
# version's files are removed first); a URL change alone (mirror) does not
# reinstall. Bookkeeping lives in <packages>/.3p/.
include_guard()

include(Variables)
include(Python)

set(SL_3P_MANIFEST "${CMAKE_SOURCE_DIR}/cmake/3p_manifest.json")
set(SL_3P_TRACK_DIR "${PREBUILT_INSTALL_DIR}/.3p")
set(SL_3P_FORMAT "slipstream-3p 1")

set(SL_3P_CACHE "" CACHE PATH
    "Download cache for 3p archives (default: \$SL_3P_CACHE, else ~/.cache/slipstream-3p)")
if (SL_3P_CACHE)
  set(_sl_3p_cache "${SL_3P_CACHE}")
elseif (DEFINED ENV{SL_3P_CACHE} AND NOT "$ENV{SL_3P_CACHE}" STREQUAL "")
  set(_sl_3p_cache "$ENV{SL_3P_CACHE}")
elseif (CMAKE_HOST_WIN32 AND DEFINED ENV{LOCALAPPDATA})
  set(_sl_3p_cache "$ENV{LOCALAPPDATA}/slipstream-3p")
elseif (DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
  set(_sl_3p_cache "$ENV{XDG_CACHE_HOME}/slipstream-3p")
else ()
  set(_sl_3p_cache "$ENV{HOME}/.cache/slipstream-3p")
endif ()
file(TO_CMAKE_PATH "${_sl_3p_cache}" SL_3P_CACHE_DIR)

if (WINDOWS)
  set(SL_3P_PLATFORM windows64)
elseif (DARWIN)
  set(SL_3P_PLATFORM darwin64)
else ()
  set(SL_3P_PLATFORM linux64)
endif ()

# A packages dir written by autobuild (or the old fetch_3p.py) has no bookkeeping in
# our format: start it over once, or every package would collide with its old copy.
if (NOT EXISTS "${SL_3P_TRACK_DIR}/format" AND EXISTS "${PREBUILT_INSTALL_DIR}")
  message(STATUS "3p: ${PREBUILT_INSTALL_DIR} predates the CMake 3p installer; recreating it")
  file(REMOVE_RECURSE "${PREBUILT_INSTALL_DIR}")
endif ()
if (NOT EXISTS "${SL_3P_TRACK_DIR}/format")
  file(MAKE_DIRECTORY "${SL_3P_TRACK_DIR}")
  file(WRITE "${SL_3P_TRACK_DIR}/format" "${SL_3P_FORMAT}\n")
endif ()

# Reconfigure when the manifest changes.
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SL_3P_MANIFEST}")

# Compute <algo> digest of <file> into <out> (lowercase hex). CMake has MD5/SHA*;
# anything else (autobuild's blake2b) goes through Python's hashlib.
function(_sl_3p_digest algo file out)
  string(TOUPPER "${algo}" _up)
  if (_up MATCHES "^(MD5|SHA1|SHA224|SHA256|SHA384|SHA512|SHA3_224|SHA3_256|SHA3_384|SHA3_512)$")
    file(${_up} "${file}" _digest)
  else ()
    execute_process(
      COMMAND "${PYTHON_EXECUTABLE}" -c
              "import hashlib,sys\nh=hashlib.new(sys.argv[1])\nwith open(sys.argv[2],'rb') as f:\n    for b in iter(lambda: f.read(1<<20), b''): h.update(b)\nprint(h.hexdigest())"
              "${algo}" "${file}"
      OUTPUT_VARIABLE _digest OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _rc)
    if (NOT _rc EQUAL 0)
      message(FATAL_ERROR "3p: cannot compute ${algo} of ${file} with ${PYTHON_EXECUTABLE}")
    endif ()
  endif ()
  string(TOLOWER "${_digest}" _digest)
  set(${out} "${_digest}" PARENT_SCOPE)
endfunction()

# Set <out> to TRUE when <file> matches the manifest's SHA256 (preferred) or its
# recorded 'hash' (hash_algorithm).
function(_sl_3p_verify file algo hash sha256 out)
  if (sha256)
    _sl_3p_digest(SHA256 "${file}" _got)
    set(_want "${sha256}")
  else ()
    _sl_3p_digest("${algo}" "${file}" _got)
    set(_want "${hash}")
  endif ()
  string(TOLOWER "${_want}" _want)
  if (_got STREQUAL _want)
    set(${out} TRUE PARENT_SCOPE)
  else ()
    set(${out} FALSE PARENT_SCOPE)
  endif ()
endfunction()

# Locate (or download) the verified archive for <name>; path in <out>.
function(_sl_3p_fetch name url algo hash sha256 out)
  string(REGEX REPLACE "[?#].*$" "" _u "${url}")
  get_filename_component(_base "${_u}" NAME)
  # LEGACY: also look (read-only) in the checkout's old .autobuild-installables[-win]
  # caches, so an existing checkout neither re-downloads ~1 GB nor depends on
  # upstream URLs that have since disappeared. CI passes SL_3P_CACHE explicitly (to
  # those same dirs), so this fallback only matters for older local checkouts; drop
  # it once those caches are migrated to SL_3P_CACHE.
  set(_candidates "${SL_3P_CACHE_DIR}/${_base}" "${SL_3P_CACHE_DIR}/${name}-${_base}")
  foreach (_legacy .autobuild-installables .autobuild-installables-win)
    list(APPEND _candidates "${CMAKE_SOURCE_DIR}/../${_legacy}/${_base}"
                            "${CMAKE_SOURCE_DIR}/../${_legacy}/${name}-${_base}")
  endforeach ()
  foreach (_candidate IN LISTS _candidates)
    if (EXISTS "${_candidate}")
      _sl_3p_verify("${_candidate}" "${algo}" "${hash}" "${sha256}" _ok)
      if (_ok)
        set(${out} "${_candidate}" PARENT_SCOPE)
        return()
      endif ()
      message(WARNING "3p: ${_candidate} does not match the manifest hash; ignoring it")
      string(FIND "${_candidate}" "${SL_3P_CACHE_DIR}/" _in_cache)
      if (_in_cache EQUAL 0)
        file(REMOVE "${_candidate}")
      endif ()
    endif ()
  endforeach ()

  file(MAKE_DIRECTORY "${SL_3P_CACHE_DIR}")
  string(RANDOM LENGTH 8 _rnd)
  set(_part "${SL_3P_CACHE_DIR}/${_base}.part-${_rnd}")
  message(STATUS "3p: downloading ${name} <- ${url}")
  file(DOWNLOAD "${url}" "${_part}"
       STATUS _status
       TLS_VERIFY ON
       INACTIVITY_TIMEOUT 120
       HTTPHEADER "Accept: application/octet-stream")
  list(GET _status 0 _code)
  if (NOT _code EQUAL 0)
    list(GET _status 1 _msg)
    file(REMOVE "${_part}")
    message(FATAL_ERROR "3p: failed to download ${name} from ${url}: ${_msg}")
  endif ()
  _sl_3p_verify("${_part}" "${algo}" "${hash}" "${sha256}" _ok)
  if (NOT _ok)
    file(REMOVE "${_part}")
    message(FATAL_ERROR "3p: ${name} downloaded from ${url} does not match the manifest hash")
  endif ()
  file(RENAME "${_part}" "${SL_3P_CACHE_DIR}/${_base}")
  set(${out} "${SL_3P_CACHE_DIR}/${_base}" PARENT_SCOPE)
endfunction()

# Remove the files a previous install of <name> put down.
function(_sl_3p_uninstall name)
  set(_list "${SL_3P_TRACK_DIR}/${name}.files")
  if (EXISTS "${_list}")
    message(STATUS "3p: removing the previously installed ${name}")
    file(STRINGS "${_list}" _files)
    foreach (_f IN LISTS _files)
      file(REMOVE "${PREBUILT_INSTALL_DIR}/${_f}")
    endforeach ()
  endif ()
  file(REMOVE "${_list}" "${SL_3P_TRACK_DIR}/${name}.installed" "${SL_3P_TRACK_DIR}/${name}.xml")
endfunction()

function(sl_3p_install name)
  get_property(_done GLOBAL PROPERTY "SL_3P_DONE_${name}")
  if (_done)
    return()
  endif ()

  get_property(_json GLOBAL PROPERTY SL_3P_MANIFEST_JSON)
  if (NOT _json)
    file(READ "${SL_3P_MANIFEST}" _json)
    set_property(GLOBAL PROPERTY SL_3P_MANIFEST_JSON "${_json}")
  endif ()
  string(JSON _pkg ERROR_VARIABLE _err GET "${_json}" packages "${name}")
  if (_err)
    message(FATAL_ERROR "3p: package '${name}' is not in ${SL_3P_MANIFEST} "
                        "(add it with scripts/3p/manifest.py add)")
  endif ()
  string(JSON _arc ERROR_VARIABLE _err GET "${_pkg}" platforms ${SL_3P_PLATFORM})
  if (_err)
    string(JSON _arc ERROR_VARIABLE _err GET "${_pkg}" platforms common)
    if (_err)
      message(FATAL_ERROR "3p: package '${name}' has no ${SL_3P_PLATFORM} or common archive")
    endif ()
  endif ()
  string(JSON _url GET "${_arc}" url)
  string(JSON _hash GET "${_arc}" hash)
  string(JSON _algo GET "${_arc}" hash_algorithm)
  string(JSON _sha256 ERROR_VARIABLE _err GET "${_arc}" sha256)
  if (_err)
    set(_sha256 "")
  endif ()

  set(_key "${_algo}:${_hash}")
  set(_stamp "${SL_3P_TRACK_DIR}/${name}.installed")
  if (EXISTS "${_stamp}")
    file(READ "${_stamp}" _installed)
    string(STRIP "${_installed}" _installed)
    if (_installed STREQUAL _key)
      set_property(GLOBAL PROPERTY "SL_3P_DONE_${name}" TRUE)
      return()
    endif ()
  endif ()
  _sl_3p_uninstall(${name})

  _sl_3p_fetch(${name} "${_url}" "${_algo}" "${_hash}" "${_sha256}" _archive)

  message(STATUS "3p: installing ${name}")
  set(_stage "${CMAKE_BINARY_DIR}/3p-stage/${name}")
  file(REMOVE_RECURSE "${_stage}")
  file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_stage}")

  # autobuild kept each archive's metadata out of the tree; so do we.
  if (EXISTS "${_stage}/autobuild-package.xml")
    file(RENAME "${_stage}/autobuild-package.xml" "${SL_3P_TRACK_DIR}/${name}.xml")
  endif ()

  file(GLOB_RECURSE _files LIST_DIRECTORIES false RELATIVE "${_stage}" "${_stage}/*")
  set(_conflicts "")
  foreach (_f IN LISTS _files)
    if (EXISTS "${PREBUILT_INSTALL_DIR}/${_f}" AND NOT IS_DIRECTORY "${PREBUILT_INSTALL_DIR}/${_f}")
      list(APPEND _conflicts "${_f}")
    endif ()
  endforeach ()
  if (_conflicts)
    file(REMOVE_RECURSE "${_stage}")
    file(REMOVE "${SL_3P_TRACK_DIR}/${name}.xml")
    list(JOIN _conflicts "\n  " _conflicts)
    message(FATAL_ERROR "3p: package '${name}' would overwrite files another package installed:\n  ${_conflicts}")
  endif ()

  file(MAKE_DIRECTORY "${PREBUILT_INSTALL_DIR}")
  file(COPY "${_stage}/" DESTINATION "${PREBUILT_INSTALL_DIR}")
  file(REMOVE_RECURSE "${_stage}")

  # Windows target on a case-sensitive host (the clang-cl cross build): LL's CMake and
  # the manifest name libraries in whatever case they like (openal32.dll vs
  # OpenAL32.dll), so add an all-lowercase link beside every mixed-case lib/ and bin/
  # file, as Windows' case-insensitive filesystem would resolve it.
  if (WINDOWS AND NOT CMAKE_HOST_WIN32)
    foreach (_f IN LISTS _files)
      if (_f MATCHES "^(lib|bin)/")
        get_filename_component(_dir "${_f}" DIRECTORY)
        get_filename_component(_fname "${_f}" NAME)
        string(TOLOWER "${_fname}" _lower)
        if (NOT _lower STREQUAL _fname AND NOT EXISTS "${PREBUILT_INSTALL_DIR}/${_dir}/${_lower}")
          file(CREATE_LINK "${_fname}" "${PREBUILT_INSTALL_DIR}/${_dir}/${_lower}" SYMBOLIC)
          list(APPEND _files "${_dir}/${_lower}")
        endif ()
      endif ()
    endforeach ()
  endif ()

  list(JOIN _files "\n" _list)
  file(WRITE "${SL_3P_TRACK_DIR}/${name}.files" "${_list}\n")
  file(WRITE "${_stamp}" "${_key}\n")
  set_property(GLOBAL PROPERTY "SL_3P_DONE_${name}" TRUE)

  # find_library()/find_path() cache each directory's listing for the rest of the
  # configure run, keyed on the directory's mtime in whole seconds. Packages share
  # directories (lib/release, include, ...), so if another package's find_library()
  # listed one in the same second this install added files to it, the new files would
  # stay invisible (e.g. libcollada14dom.a). Let the clock tick past the second of our
  # last write before anything else runs; this only costs time when a package is
  # actually (re)installed. (autobuild never hit this: each install was a separate,
  # slower process.)
  string(TIMESTAMP _t "%s")
  while (TRUE)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
    string(TIMESTAMP _now "%s")
    if (NOT _now STREQUAL _t)
      break()
    endif ()
  endwhile ()
endfunction()

macro (use_prebuilt_binary _binary)
  sl_3p_install(${_binary})
endmacro ()

#Sadly we need a macro here, otherwise the return() will not properly work
macro ( use_system_binary package )
  if( USE_CONAN )
    target_link_libraries( ll::${package} INTERFACE CONAN_PKG::${package} )
    foreach( extra_pkg "${ARGN}" )
      if( extra_pkg )
        target_link_libraries( ll::${package} INTERFACE CONAN_PKG::${extra_pkg} )
      endif()
    endforeach()
    return()
  endif()
endmacro()

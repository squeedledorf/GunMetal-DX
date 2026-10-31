# Fuzzing the untrusted-input parsers

libFuzzer harnesses (clang, ASan + UBSan) for the code that parses bytes the viewer did not
produce: UDP datagrams, LLSD, mesh and image assets, notecards, material JSON. Opt-in, Linux +
clang only; it never touches the normal build or the Windows cross build.

## Build

The fuzz build instruments the **whole tree** (so bugs inside llcommon / llmessage / llimage are
found, not only in the harness files). It needs its own build directory and clang.

```sh
cmake -S indra --preset linux-fuzz          # = CC=clang CXX=clang++, -DLL_FUZZ=ON, no -Werror, no PCH
ninja -C build-linux-fuzz fuzz_all          # builds the harnesses only (not the viewer)
# or: cmake --build build-linux-fuzz --target fuzz_all
# (`cmake --build --preset linux-fuzz` also works, but only with indra/ as the working directory)
```

Binaries land in `build-linux-fuzz/fuzz/`. Do not run plain `ninja` in that directory: it would
also build the (sanitised) viewer. 3p libraries (OpenJPEG, libpng, libjpeg, expat, ...) are the
uninstrumented prebuilts, so a bug inside them is only caught when it corrupts memory ASan
intercepts (the heap, memcpy, ...); coverage guidance stops at their boundary.

## Run

```sh
scripts/fuzz/run.sh list                       # what is built
scripts/fuzz/run.sh llsd_xml 300               # fuzz one target for 5 minutes
scripts/fuzz/run.sh all 60                     # every target, a minute each
scripts/fuzz/run.sh check                      # replay all seeds once (seconds; a smoke test)
FUZZ_EXTRA="-jobs=8 -workers=8" scripts/fuzz/run.sh message 1800
```

State (grown corpus, crash artifacts) is under `build-linux-fuzz/fuzz-work/<name>/`; the
checked-in seeds in `indra/fuzz/corpus/<name>/` are read, never written. The script sets
`ASAN_OPTIONS=protect_shadow_gap=0:detect_leaks=0:...` (NVIDIA maps into ASan's shadow gap) and
`UBSAN_OPTIONS=halt_on_error=1:suppressions=indra/fuzz/ubsan.supp`. `FUZZ_LOG=1` sends
`LL_WARNS` to stderr when debugging an input.

An `LL_ERRS` reachable from input aborts the harness (`fuzz_common.h`): in the viewer that is a
remote crash, so it is a finding. A tiny input that makes a decoder allocate gigabytes trips
`-rss_limit_mb` (default 2048, `FUZZ_RSS_MB`); also a finding, because the texture fetcher feeds
these decoders server data.

## When it finds something

```sh
scripts/fuzz/run.sh repro llsd_xml build-linux-fuzz/fuzz-work/llsd_xml/crashes/crash-<sha>   # reproduce
scripts/fuzz/run.sh minimize llsd_xml build-linux-fuzz/fuzz-work/llsd_xml/crashes/crash-<sha> # -> crash-<sha>.min
```

Fix the parser, then keep the input as a regression: copy the `.min` file into
`indra/fuzz/corpus/<name>/crash-<short-description>.<ext>` (it is replayed by
`run.sh check` from then on), and where the bug is in pure logic also turn it into a unit test
(see the test-framework tickets). Run `scripts/fuzz/run.sh merge <name>` now and then to shrink the
work corpus; copy new interesting minimal inputs back into the seed directory sparingly (keep
seeds small: they are in git).

## Harnesses

| Target | Entry point | Input |
| --- | --- | --- |
| `fuzz_llsd_xml` | `LLSDSerialize::fromXML` (+ format and re-parse) | LLSD XML |
| `fuzz_llsd_notation` | `LLSDSerialize::fromNotation` (+ round trip) | LLSD notation |
| `fuzz_llsd_binary` | `LLSDSerialize::fromBinary` (+ round trip) | LLSD binary |
| `fuzz_llsd_auto` | `LLSDSerialize::deserialize` (header sniffing) | any of the three |
| `fuzz_unzip_llsd` | `LLUZipHelper::unzip_llsd` | zlib'd binary LLSD (skin / decomposition / physics / LOD blobs) |
| `fuzz_message` | ack trim, `LLMessageSystem::zeroCodeExpand`, `LLTemplateMessageReader::validateMessage` + `readMessage` with the real `message_template.msg` | byte 0 bit 0 = trusted circuit, then the datagram |
| `fuzz_mesh_lod` | `LLVolume::unpackVolumeFaces(U8*, S32)` | a mesh LOD / physics block |
| `fuzz_mesh_asset` | `LLModel::loadModel` (header, skin, decomposition, LOD via header offsets) | byte 0 = LOD, then the asset |
| `fuzz_mesh_skin` | `LLMeshSkinInfo`, `LLModel::Decomposition` from a decoded LLSD | byte 0 bit 0 = skin / decomposition, then zlib'd LLSD |
| `fuzz_image_tga` `_png` `_bmp` `_jpeg` `_tiff` `_j2c` | `LLImageFormatted::updateData` + `decode` (J2C through OpenJPEG) | the image file |
| `fuzz_gltf_material` | `LLGLTFMaterial::fromJSON`; `applyOverrideLLSD` | byte 0 bit 0 = JSON / LLSD override |
| `fuzz_notecard` | `LLNotecard::importStream` (embedded items included) | notecard text |
| `fuzz_inventory` | `LLInventoryItem` / `LLInventoryCategory::importLegacyStream` | byte 0 bit 0 = item / category, then text |

Seeds live in `corpus/<name>/` (`<name>` = the target without `fuzz_`); the binary ones come
from `make_seeds.py` (standard library; Pillow for the PNG / JPEG / J2C ones). Dictionaries are in
`dict/<name>.dict` for the text formats (LLSD XML / notation / binary, notecard, inventory, glTF).

## Not covered, and why

* **SLBridge** (`slbridge.cpp processOwnerSay`) and the **weather parcel tag parser**
  (`slweatherregion.cpp parseTag` / `findTagSpan`): both are in `newview` behind
  `llviewerprecompiledheaders.h` and viewer globals (`gObjectList`, `LLViewerParcelMgr`, ...), so a
  harness would pull in the whole viewer. Refactor proposed: move the line split of
  `processOwnerSay` (prefix, type, `|` fields) into a viewer-free `SLBridgeLine::parse`, and move
  `parseTag` / `findTagSpan` / `formatTag` and their number parsers into `slweathertag.cpp`
  that needs only `slweather.h` (`SLWeather::Target` is plain data; the parse code in
  `slweatherregion.cpp` lines ~30-110 and ~295-450 uses nothing else but `llclamp` from llmath,
  while `update()` is where `gAgent` / `LLViewerParcelMgr` / `LLEnvironment` come in), leaving
  `SLWeatherRegion::parseTag` as a thin forwarder. A harness can then compile those files
  directly. The same split makes both unit-testable.
* **Mesh header** (`LLMeshHeader::fromLLSD`, `strip_deprecated_header` in `llmeshrepository.cpp`):
  newview only. Proposal: move both to `llprimitive` beside `LLMeshSkinInfo`. The header is
  binary LLSD, which `fuzz_llsd_binary` and `fuzz_mesh_asset` already reach; what is missing is
  the offset / size bookkeeping in `headerReceived`.
* **glTF asset / scene loader** (`newview/gltf/asset.h`): includes `llviewertexture.h`. The
  JSON parse (boost::json) would need to be separable from the texture upload.
* **DXT / DDS** (`LLImageDXT`): `updateData()` and `getMipOffset()` raise `LL_ERRS` on a short
  or old-format header, so the harness would stop on almost every input until those fail soft;
  it only reads local files. `fuzz_image.cpp` already supports `IMG_CODEC_DXT`; add it to the
  codec list in `CMakeLists.txt` once that is fixed.
* **Message system**: circuit lookup, duplicate-resend suppression and `LLPacketRing::receivePacket`
  are not exercised; the ack-count trim and zero-code expansion are. Handlers are not registered,
  so what the viewer does with a decoded message is out of scope.
* **Other candidates** (not requested, easy to add with the same pattern): settings / environment
  LLSD (`LLSettingsSky::fromLLSD`), `LLUri` / `LLUriParser`, `LLCacheStore` records, collada
  (`lldaeloader`), `LLParcel` LLSD.

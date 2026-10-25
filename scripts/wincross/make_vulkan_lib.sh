#!/bin/bash
# The Windows SDK has no vulkan-1.lib: build the import library from the Vulkan
# loader's export list (the same vk* API on every platform) and stage the headers.
# usage: make_vulkan_lib.sh <out_dir>
set -euo pipefail
OUT=$1
mkdir -p "$OUT/include"
cp -r /usr/include/vulkan /usr/include/vk_video "$OUT/include/" 2>/dev/null || cp -r /usr/include/vulkan "$OUT/include/"
LOADER=
for f in /usr/lib/libvulkan.so.1 /usr/lib/x86_64-linux-gnu/libvulkan.so.1 /usr/lib64/libvulkan.so.1; do
  [ -f "$f" ] && { LOADER=$f; break; }
done
[ -n "$LOADER" ] || { echo "make_vulkan_lib: Vulkan loader (libvulkan.so.1) not found" >&2; exit 1; }
{ echo "LIBRARY vulkan-1.dll"; echo "EXPORTS"; nm -D --defined-only "$LOADER" | awk '$2=="T" && $3 ~ /^vk/ {print $3}'; } > "$OUT/vulkan-1.def"
llvm-dlltool -m i386:x86-64 -d "$OUT/vulkan-1.def" -l "$OUT/vulkan-1.lib"

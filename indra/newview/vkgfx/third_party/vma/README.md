# Vulkan Memory Allocator (vendored)

- Upstream: https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
- Release: **v3.4.0** (latest tagged release on 2026-10-05), commit
  `3aa921224c154a0d2c43912bc88e1c42ce1f7607`
- Files: `include/vk_mem_alloc.h` (local patches below), `LICENSE.txt` (MIT); `pool_sim.cpp` is ours
  (a standalone measurement, not built with the viewer)
- sha256(vk_mem_alloc.h) = `8487b7995ad3b263eb73bc5b9a77d71aa69b6bef5d58a715c02d2663afd81f1a`

Used by `vkgfx/vk_device.cpp` for texture suballocation (#411). Include it only through
`vkgfx/vk_vma.h` (silences the header's third-party warnings); the implementation is
compiled once, in `vkgfx/vk_vma.cpp`. Vulkan entry points are fetched at runtime
(`VMA_DYNAMIC_VULKAN_FUNCTIONS`, from `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr`).

To update: replace both files from the new tag, update the tag / commit / hash above
(the hash is of the upstream file), then re-apply the local patches. Each is marked
`SLIPSTREAM` in the header; keep the list complete:
- #587 `VmaBlockVector::IncrementallySortBlocks`: a full insertion sort (MIN_MEMORY
  placement relies on fullest-first block order).
- #614 `vmaReleasePoolEmptyBlocks` / `VmaBlockVector::ReleaseEmptyBlocks`: free a pool's
  empty blocks now (the one-block hysteresis), used by texture pool compaction under VRAM
  pressure.

## Texture pool sizing (#411): measured, not assumed

Standalone simulation on the RTX 5090 (driver 615.71) with this VMA: real
`vkCreateImage` + `vkGetImageMemoryRequirements2` (BC7 / BC1 / RGBA8, full mips,
mutable format), pools exactly as `VkDeviceContext::initTexturePools` builds them
(one pool per slab class, one TLSF medium pool, explicit block sizes,
`STRATEGY_MIN_MEMORY`), dedicated images counted at the 64 KiB rounding.
Columns = VRAM (MB, blocks + dedicated) after each phase:

1. allocate (20k images, or 14k for the "ticket" mix: ~72 % <= 32 px);
2. 100k churn (random image replaced by a new random one: re-creates);
3. "ladder": 60 % of images re-created one mip level down (new first, then free);
4. unload 50 % at random;
5. 20k more churn;
6. free everything (what VMA keeps: one empty block per pool).

Realistic mix (20k images, 1.25 GB requested):

| config (small blocks / medium cap / medium blocks) | alloc | churn | ladder | unload | churn | empty |
|---|---|---|---|---|---|---|
| dedicated (no pools)                | 2327 | 2226 | 1855 | 903  | 1113 | 0  |
| 4 MiB / none (small slabs only)     | 1362 | 1266 | 862  | 483  | 705  | 32 |
| **4 MiB / 256 KiB / 8 MiB (default)** | 1322 | 1233 | 871  | 557  | 759  | 40 |
| 4 MiB / 1 MiB / 16 MiB              | 1296 | 1277 | 1014 | 795  | 922  | 48 |
| 4 MiB / 4 MiB / 64 MiB              | 1299 | 1414 | 1277 | 1175 | 1253 | 96 |
| 16 MiB / 4 MiB / 64 MiB (ticket plan) | 1363 | 1474 | 1329 | 1227 | 1297 | 192 |

Ticket mix (14k images, 10k tiny):

| config | alloc | churn | ladder | unload | churn | empty |
|---|---|---|---|---|---|---|
| dedicated                    | 1041 | 1038 | 968 | 487 | 527 | 0   |
| 4 MiB / none                 | 312  | 307  | 223 | 167 | 203 | 32  |
| **4 MiB / 256 KiB / 8 MiB**  | 294  | 300  | 241 | 198 | 223 | 40  |
| 4 MiB / 4 MiB / 64 MiB       | 288  | 352  | 356 | 356 | 280 | 96  |
| 16 MiB / 4 MiB / 64 MiB      | 336  | 400  | 400 | 400 | 336 | 192 |

What it shows:
- Suballocation removes the padding: allocation-time VRAM drops 40-70 %, and driver
  allocations fall from 20000 to ~1050 with the default (44 blocks + ~1000 images
  above 256 KiB that stay dedicated; 13-50 blocks across the configs).
- A block is released only when every image in it is gone. Unloads and mip-downs free
  images scattered over all blocks, so big blocks strand memory: with 64 MiB medium
  blocks the ladder's mip-downs freed almost nothing (ticket mix 352 -> 356 MB) and a
  50 % unload left more VRAM in use than no pooling at all (1175 vs 903 MB). Fullest-
  block-first placement of re-created images does not drain blocks fast enough on its
  own (VMA keeps blocks only approximately sorted: one bubble swap per operation).
- Small blocks + a modest medium cap keep almost all of the padding win and still
  return memory on the ladder / unload. Phase 2 (defragmentation) is what would let
  the large medium tier work.
- VMA keeps one empty block per pool as hysteresis (freed on that pool's next free
  that doesn't empty another block): 32-192 MB after freeing everything, by config.
- Correlated unload (freeing the oldest 50 %) behaved like random unload after churn:
  placement is scrambled by re-creates.
- No image of these kinds reported prefersDedicatedAllocation on this driver.

The defaults are the settings RenderVKTexturePoolSmallBlockMB (4),
RenderVKTexturePoolMediumMaxKB (256), RenderVKTexturePoolMediumBlockMB (8); the
ticket plan is 4096 / 64. Source of the simulation: `pool_sim.cpp` next to this file
(build: `g++ -O2 -std=c++17 pool_sim.cpp -lvulkan`; env NOPOOL, SMBLK, MEDMAX, MEDBLK,
TICKET, CORR).

# Contributing to Slipstream

Thanks for your interest in Slipstream. This page covers bug reports, feature requests and code
contributions.

## Where things happen

- **Issue tracker:** <https://git.slipstreamviewer.org/sky_line/Slipstream/issues>. Bugs,
  feature requests and development work are all tracked here.
- **Help wiki:** <https://slipstreamviewer.org/wiki/>. The wiki source is in the
  [slipstream-website](https://git.slipstreamviewer.org/sky_line/slipstream-website) repository.

## Reporting bugs

The wiki's [reporting bugs](https://slipstreamviewer.org/wiki/reporting-bugs/) page lists what to
include: the version from **Help → About**, your OS and graphics card, steps to reproduce, and your
`Slipstream.log`. For crashes, see the
[crash reports](https://slipstreamviewer.org/wiki/crash-reports/) page.

Slipstream contains a lot of code inherited from Linden Lab's viewer. If a problem also happens in the
[official Second Life viewer](https://releasenotes.secondlife.com/viewer.html), please report it to
Linden Lab at [feedback.secondlife.com](https://feedback.secondlife.com/bug-reports) too.

## Code contributions

- Open or comment on an issue first, so we can agree on the approach before you write the code.
- Rendering work targets the Vulkan renderer (`indra/newview/vkgfx`, shaders in
  `indra/vkclean/shaders`). The renderer is GPU-driven: new render paths use the persistent instance
  buffer, bindless materials, compute culling and indirect draws, not per-object binds or draws.
- After editing a shader in `indra/vkclean/shaders`, regenerate the embedded SPIR-V with
  `python3 indra/vkclean/regen_world_spv.py` and commit both.
- Build and test locally (`make`, then `make run`), and check the Windows cross-build (`make windows`)
  if you touch platform code.
- Reference the issue in your commit messages (`Refs #123` / `Fixes #123`).
- Keep changes compliant with Linden Lab's
  [Third Party Viewer Policy](https://secondlife.com/corporate/third-party-viewers).

By contributing you agree that your contribution is licensed under the LGPL 2.1, like the rest of the
viewer.

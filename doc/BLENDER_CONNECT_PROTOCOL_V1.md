# Blender Connect file protocol — version 1

Blender Connect is an opt-in, local-only preview. The viewer imports one explicitly
selected GLB through the normal mesh importer and own-avatar attachment path. It sends
no asset, inventory or simulator mutation. The sender is a separate GPL add-on; no
add-on code or Python source check is included in the viewer repository.

## Files and publication

The sender atomically replaces `preview.glb`, then atomically publishes
`preview.glb.blender-connect.json` in the same private workspace:

```json
{"protocol":"slipstream-blender-connect","version":1,"session":"0123456789abcdef0123456789abcdef","revision":1,"digest":"40 lowercase SHA-1 hex digits","state":"active"}
```

The session is a random 32-hex identity. Revision is a positive signed 64-bit integer,
increasing on every successful publication, including equal-size changes within one
filesystem timestamp tick. Digest is SHA-1 of the complete GLB, used for publication
consistency, not authentication. Same-user local processes are the trust domain.
The JSON is at most 1024 bytes. Active senders refresh it at least once per second.

A missing, invalid, future-dated or 15-second-old heartbeat **pauses** imports and
retains the last accepted preview, its placement and attachment. A fresh heartbeat
resumes automatically. The sender explicitly stops by writing `"state":"stopped"`;
this marker remains available after add-on shutdown. The viewer's Disconnect / Remove
also explicitly removes the preview. Transient read races retry with bounded
1/2/4/8-second backoff; a stable content rejection waits for a new revision.

The viewer writes `preview.glb.blender-connect.verdict.json` with protocol, version,
revision and a short `reason` (`Accepted` or a safe rejection). The sender displays
only matching-revision verdicts and tolerates partial feedback reads. The viewer is
the authoritative validator; the sender does not duplicate its GLB validation rules.

## Validation and resource lifetime

The production `SLBlenderConnect::load` entry validates bytes and decodes embedded
resources. The mesh importer additionally validates its asset representation before
material commit. Limits: GLB 2, one JSON/BIN pair, 32 MiB total, 512 KiB JSON, depth 32,
128 object members, 4096 array elements, one fully reachable node forest with unique
roots/parents and depth 128. Expanded mesh-node uses are limited to 500,000 vertices,
1,500,000 indices and 128 estimated parts. There are at most 16 materials, 32 PNG/JPEG
images, 4096 pixels per axis and 32 megapixels total. Buffer/accessor ranges, triangle
indices, texture references and skin references are checked before use.

URIs (including data URIs), sparse accessors, morph targets, animation and unsupported
geometry/texture-source extensions are rejected. Optional unknown **material**
extensions are ignored; unsupported `extensionsRequired` entries are rejected.
Use triangles and ordinary metallic/roughness materials. Compatible rigging is the
existing upload/local-mesh importer behavior, not arbitrary skeleton retargeting.
Creator-authored LOD chains, physics shapes and Blender animation streaming are absent.

Immutable, noncopyable snapshots bind geometry and materials from one revision.
Content hashes reuse material IDs across geometry-only revisions and fetched images
across materials/revisions. The image lookup has at most 128 weak identities and does
not pin residency; material ownership retires on replacement/disconnect. Cluster keys
are transient only for Blender Connect; late builds cannot write either disk backend.
There is no local-mesh mirror-recovery branch: transient payloads remain in memory.

Private temporary roots have an application/version marker and an exclusive OS owner
lock. Startup cleanup removes only matching roots whose lock can be acquired, proving
the owner inactive. It never relies on age/prefix alone and never removes an active
viewer/add-on workspace. Snapshots retain their owner through in-flight imports.

## Verification and integration

Production-validator unit tests and libFuzzer integration remain pending separate
integration. Draft tests and synthetic seeds are available locally for the maintainer;
they are not registered or included in this viewer-side patch. Synthetic seeds are not
a substitute for real Blender exports. No passing test or end-to-end result is claimed.

The UI uses cached controls and manager change events. Normal Local Mesh retains its
three-second file polling, ordinary cluster-cache policy and prior respawn semantics.
Regression checks should cover ordinary DAE/FBX/GLB loading, reload, multi-part transforms
and attachments alongside Blender Connect's placement/attachment and material reuse.

Before release: full Linux/Windows viewer builds, Blender headless smoke, and in-world
testing with a real SL rig remain required. Check material/image edits versus geometry
edits, mode exit and explicit Send, pause/resume with an unchanged revision, coarse
timestamps, malformed updates, deletion, Stop, disconnect during import, logout,
parallel active owner locks, non-ASCII Windows paths and cluster settings. No viewer
launch is part of source/unit/fuzz validation.

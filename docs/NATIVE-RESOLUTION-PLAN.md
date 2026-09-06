# Render at the window's native resolution

Planning snapshot: 5 September 2026. The design investigation below predates
Claude's final aspect commit `8493205`; its current-code table is historical.

Implementation now provides opt-in `PSPRECOMP_RESOLUTION=window`, separate guest
and physical dimensions, float transformed positions, compatible GPU texture
views, precise CPU-write imports and resize migration of color/depth/stencil.
The game's body font retains its bitmap stroke weight through a host-side
sampling exception. Physical-resolution point/line coverage is still pending.
It extends Claude's final scene/HUD viewport mapping rather than the earlier
safe-area squeeze described below. His window sizing and screenshot controls
also supplied the capture prerequisite. See [current validation and remaining
work](RENDER-CHECKS.md#window-resolution-rendering). The default remains `psp`.

The intended result is actual rasterization at the window's physical pixel
dimensions. At a 1920x1080 drawable with adaptive aspect enabled, the visible
scene should be rendered at 1920x1080. The PSP address space, framebuffer stride,
texture assets and game timing retain their existing meanings.

## Recommendation

Extend the existing GL backend with separate guest and GPU target dimensions.
Keep the existing 1x path available as the software-comparison reference. Start
with a deterministic 2x implementation, then add arbitrary drawable sizes and
resize handling. Preserve float positions before GE quantization as an optional
enhanced geometry path. These are several reviewable increments, not a window
size constant change.

The most consequential dependency is framebuffer ownership: the display pair
must retain high-resolution contents across compositing passes, while byte
reinterpretation and CPU access still see valid PSP memory. Prove those paths
before making automatic window resolution the default.

## What the code does today

| Area | Current behavior | Consequence |
| --- | --- | --- |
| `host/present.c`: `present_gl_drawable_size` | SDL publishes physical drawable width and height through two atomics. | High-DPI size discovery already exists; a coherent, frame-latched snapshot is still needed for resizing render targets. |
| `host/replacements.c`: `psp_func_000889B4` | Claude's patch temporarily lends the camera rebuild a virtual width derived from window aspect. | Camera projection and frustum belong to the aspect feature. GPU resolution should not independently change guest dimensions or FOV. |
| `host/render_gl.c`: `aspect_safe_area` | Claude's patch adjusts screen-space geometry in logical PSP coordinates, with full-frame and scratch-target exceptions. | Preserve this classification and apply its transform exactly once when adding physical raster scale. |
| `rendertarget`, `rt_prepare`, `flush` | One `w,h` pair describes guest extent, GL attachments, viewport and shader coordinates. Targets retain their first storage even if their layout later changes. | Split the coordinate spaces and implement target reconfiguration. |
| `gl_present` | Blits the current GE target's visible 480x272 region into the drawable, then reads dirty targets back to guest memory. | The larger window currently contains an enlarged PSP image. The presentation API does not pass the actual scanout address. |
| `texcache_get`, fragment shader | Matching RGBA8888 targets can be sampled directly; other target layouts go through guest bytes and the shared decoder. Sampling uses logical texel coordinates and `textureSize`. | A scaled attachment cannot transparently replace a guest-sized texture in this shader. |
| `readback_rt` | Reads the entire GL attachment, flips rows and packs bytes using guest format/stride. | Enlarging its dimensions directly would break guest writes and multiply CPU transfer volume. |
| `ge.c`: `emit_tri`, `emit_point_line`, transformed decode | Computes floating-point projection, then stores positions rounded to 1/16 PSP pixel in `psp_vertex`. | GL cannot recover the precision already discarded. |
| `push_point_sample`, `psp_render_walk_line` | Rasterizes lines as PSP pixel samples, emitted as unit quads. | Enlarging those quads retains a coarse staircase even when triangles improve. |

Source context: [M7 in the roadmap](ROADMAP.md#m7--the-pc-ports-own-settings)
and [renderer checks](RENDER-CHECKS.md). The latter records the scratch target
at `0x04154000` changing from 256x128/5551 to 128x64/8888 during a full run.
That history is absent from an isolated frame capture.

## Boundary with Claude's aspect work

Aspect mode chooses camera shape and HUD placement. Resolution mode chooses the
number of physical samples used to draw that composition. Both should work
independently: window resolution plus PSP aspect gives a sharp letterboxed
image; window resolution plus window aspect fills the drawable.

Use the content rectangle selected by the aspect policy as the resolution
input. In window-aspect mode it is the entire drawable. In PSP-aspect mode it
is the fitted rectangle inside the drawable. Share a coherent size/generation
snapshot across camera, HUD, target sizing and presentation; do not read new
width/height pairs independently halfway through a frame. Agree on the latch
point after inspecting the two `sceDisplaySetFrameBuf` calls per rendered frame.
A flip callback alone is not proof that a new frame started.

The current HUD pre-adjustment can still work: the physical viewport applies
the same X/Y scaling that the final enlargement applied before. Audit its
comments and tests when that scaling moves into rasterization. Fullscreen
composites, clears and fades must cover the content rectangle; partial HUD
clipping must follow the HUD transform where appropriate. Tiled fullscreen
passes need explicit coverage tests because the current bounding-box heuristic
is per draw. This is an integration contract, not a second camera replacement.

The overlapping files are `host/present.{c,h}`, `host/render_gl.c`,
`host/boot.c`, `tools/psprecomp/include/psprecomp/render.h`, and `src/hle/ge.c` in
that submodule. Re-read Claude's final changes before implementation. This plan
does not change those files or assume his current patch is final.

## Dimensions and render targets

Give each target explicit guest address, stride, format and extent, physical
attachment extent, logical-to-physical scale, content generation, and guest/GPU
ownership state. Address alone is insufficient when a surface changes format
or overlaps another surface. Recognize the display surfaces from observed
display/target metadata rather than scaling every wide allocation.

For a visible content rectangle `Cw x Ch`, use `Sx = Cw/480` and `Sy = Ch/272`
for the display surfaces. Allocate enough physical storage for the full guest
extent, including row padding: `ceil(guest_w*Sx) x ceil(guest_h*Sy)`. Keep the
explicit scale in the coordinate transform; dividing by the rounded attachment
size as though it were an exact scale introduces drift at fractional sizes.

At 1920x1080 with adaptive aspect, the 512x272 guest display attachment becomes
2048x1080, of which exactly 1920x1080 is visible. Preserve the padded region for
texture/address semantics and crop it from presentation. A 960x544 drawable
uses a 1024x544 attachment and a 960x544 visible region. At 3840x2160 the
corresponding attachment is 4096x2160.

Map viewport, clear geometry, scissor edges, stencil passes and final source
rectangles through the same transform, including top-left versus bottom-left
origin. Convert inclusive guest scissors to half-open rectangles first. Define
one pixel-center rounding convention and test adjacent boundaries at fractional
scale so they neither leak nor leave seams. The 1/256 PSP-pixel edge bias in
the current vertex shader is a 1x compatibility rule; enhanced mode needs an
explicit physical-pixel bias rather than multiplying that offset by resolution.

Initially keep the byte-reinterpreted scratch surface at 1x, including its known
format/size transitions. The display pair and compatible RGBA compositing stay
at enhanced resolution. Same-target feedback needs a source snapshot before
drawing; overlapping guest ranges also need hazard detection, not just exact
base-address matches.

## Texture sampling and guest memory

Direct render-target textures need both logical texture metadata and physical
backing metadata. Preserve wrapping, cropping, orientation and PSP texel units
in logical coordinates; convert the resulting sample location into the physical
attachment. Do not clamp against padded allocation bounds. A logical sample
must be able to reach the added physical detail: taking an integer PSP texel
first and then scaling that index would erase the benefit during compositing.
Ordinary decoded asset textures retain the current sampler.

There is already a concrete mismatch to solve: the direct-target shortcut
requires `t->h == r->h`, but GE texture sizes are powers of two (at most 512),
whereas display attachments are 272 rows tall. Thus an ordinary 512x272 display
target cannot satisfy that shortcut. Historical mission/combat replay logs in
`reports/m5-stencil-lines-final/` report every target-texture encounter taking
the alias-decode route, although a full-run trace is needed to identify each
view and consumer. Support compatible texture *views*, not only identical
extents: retain the texture's declared logical dimensions while mapping its
populated render-target region correctly. Never stretch 272 rows over a declared
512-row texture. Reads outside the GPU-owned region must obtain the appropriate
guest bytes or use an explicitly reported compatibility path. Trace offsets,
stride, format, UV range and Y orientation before choosing that mapping.

Keep the PSP AUTO/CONST/SLOPE LOD policy for the first increment. In particular,
do not accidentally change mip selection because attachment dimensions changed
or the HUD was temporarily squeezed. Once the rendering path works, assess
physical-footprint AUTO LOD for world geometry as a separate quality change;
CONST/SLOPE and HUD behavior need their own semantics. Higher resolution does
not add detail to the original texture or movie assets.

Maintain a guest-sized resolve surface for scaled targets. Resolve on the GPU
before readback so the CPU still receives only guest-sized bytes. Use an explicit
sample-selection rule for compatibility bytes, including alpha/stencil. Averaging
alpha can invent a stencil value that was never written. Diagnostic RGB
downsampling can use a different filter, but must not silently become the guest
memory representation. A downsampled enhanced image is not guaranteed to equal
an independently rasterized 1x image, even with nearest sampling.

Preserve the existing present-time synchronization and alias-triggered readback
initially. Removing synchronous readbacks is a later optimization. If a scaled
surface acquires a byte-interpreting consumer, resolving provides a defined
conversion but does not prove PSP equivalence. Pin known such surfaces to 1x
from creation; an unexpected transition must be counted and handled explicitly.
Reproducing exact earlier 1x bytes after the fact would require a parallel 1x
render or replay, and should not be promised by a simple downsample.

CPU and HLE writes need the reverse path. The current target inherits guest
contents once; the texture cache's write generations do not automatically update
an existing FBO. Trace writes into active display/auxiliary ranges, then import
modified regions before GPU drawing or sampling and before a CPU-only frame is
shown. Retain unaffected high-resolution pixels. Track the renderer's own export
generation so it is not mistaken for a new CPU write. Handle partial writes,
GE copies and format aliases before discarding pending GPU data. Existing
256-byte memory generations provide useful evidence, but do not identify write
origin or by themselves reconcile mixed GPU and CPU ownership.

Alpha/stencil import/export continues at physical resolution. Resolve guest
alpha only after exporting pending stencil; guest alpha uploads invalidate the
corresponding hardware stencil. Test destination-alpha consumers, overlapping
stencil draws and scissored alpha clears at scale.

## Geometry precision and line quality

The roadmap suggests a second path carrying untransformed vertices and matrices
to the GPU. That is a possible later architecture, but current GE code already
has float screen coordinates after its clipping and projection. A smaller first
step is an optional enhanced vertex payload retaining those values alongside
the unchanged quantized contract. GL consumes it only in enhanced mode; software
and 1x GL continue using the existing fields.

Populate the enhanced payload consistently for clipped vertices, transformed
sprites, strips, fans, immediate vertices and any tessellation output. Keep the
existing lighting, UV/W, texture Q, fog and depth behavior. Audit culling and
degenerate decisions that currently use quantized coordinates; converting fixed
coordinates back to float at the GL boundary cannot restore lost precision.
Apply the HUD transform to the float payload as well as the legacy fields.

Larger attachments alone still rasterize additional triangle samples, but the
remaining vertex grid would be 1/4 output pixel at 4x. Retaining float positions
addresses that precision limit without duplicating all GE transforms in a new
shader pipeline. This proposal refines the roadmap's implementation suggestion
while keeping its requirement to preserve the 1x oracle.

For enhanced lines, use explicit segment triangles or a deterministic walker at
physical resolution, with defined endpoint and join coverage. Preserve apparent
HUD line weight in logical UI units; blindly using one physical pixel makes the
targeting outline shrink on high-DPI screens. Avoid overlapping joins that blend
twice. Keep the existing PSP sample walker for 1x conformance. Validate points,
radar lines, targeting outlines and transformed line clipping separately.

## Resize lifecycle and configuration

Proposed controls, not implemented: `PSPRECOMP_RESOLUTION=psp|window`, defaulting
to `psp` during rollout, plus a fixed internal-size override for reproducible
enhanced-mode tests. Keep aspect mode independent. Reject incompatible explicit
backend selections and report the effective resolution, scale and fallback.
Full settings UI, supersampling and dynamic performance scaling can follow.

On the GE thread, flush the old generation, synchronize dirty ownership, allocate
new attachments, validate them, preserve existing color/depth/stencil history,
then switch both display surfaces coherently and retire old resources. Rebuild
stencil scratch and resolve resources and invalidate cached attachment bindings.
Resize must preserve previous-frame compositing data rather than produce a black
history frame. Color may be filtered when moving history; depth/stencil transfer
uses nearest selection with matching formats. Keep color alpha consistent with
the transferred stencil, and fall back to the previous working allocation if
new storage fails.

Coalesce pending resizes at the established frame boundary. Minimized or
zero-sized drawables retain valid storage and defer presentation/allocation.
Check texture, renderbuffer and viewport limits and integer/allocation bounds;
report any capped scale. Repeated resize must not leak GL objects. Also trace
which target `sceDisplaySetFrameBuf` selects: the current GL path shows
`g.cur_rt`, while the interface's `present()` has no scanout arguments. A
native-resolution path should prove or fix this mapping, especially for movie
and CPU-only frames.

Physical drawable pixels, rather than logical window units, are the appropriate
input, as specified by [SDL's drawable-size API](https://wiki.libsdl.org/SDL2/SDL_GL_GetDrawableSize).
Depth/stencil blits require nearest filtering; see the
[Khronos blit reference](https://wikis.khronos.org/opengl/GlBlitFramebuffer).

## Implementation sequence and acceptance gates

| Increment | Deliverable | Gate before proceeding |
| --- | --- | --- |
| 1. Establish ownership and observation | Record target histories, display selection, target-texture layouts, CPU writes and repeated flips. Add deterministic physical-size override and an actual GPU-output capture path. | Can identify and capture the displayed surface at requested physical size; existing capture identity and 1x results remain valid. |
| 2. Split dimensions at 1x | Separate guest/physical metadata, target lifetime, coordinate helpers and coherent presentation snapshot. Address known target reconfiguration. | Existing backend conformance and exact-capture gates pass; synthetic format/size history passes. |
| 3. Fixed 2x rendering | Scale the display pair with compatible target sampling, guest resolve/upload, stencil synchronization and visible crop. Keep byte-interpreted scratch at 1x. | Actual 960x544 detail survives compositing; aliases, CPU writes, fades and partial clears work; no guest-memory overflow. This is an integration milestone with legacy vertex precision. |
| 4. Enhanced geometry | Retain float transformed positions and implement physical-resolution line coverage. | Sub-1/16 PSP-pixel motion changes enhanced geometry smoothly; clips, UI layout, line weight and 1x output remain correct. |
| 5. Window resolution | Arbitrary X/Y scale, fractional boundaries, resize migration, high-DPI and allocation fallback. | 1080p, 1440p, 4K, ultrawide and odd-size cases render at the requested content size; repeated resize preserves history and resources. |
| 6. Validate and expose | Full-scene review, GPU/resolve/readback benchmarks, documented control and defaults. | Stable full replay, reviewed enhanced captures and measured performance; choose default only from this evidence. |

Run the existing 430-check backend fixture, runtime CTest and render-check Python
suite on the 1x path. Reuse the six exact GE captures and their reviewed baseline;
do not relax software identity guards to accommodate enhanced output. Record
aspect and resolution settings in new test results.

Enhanced validation needs GPU images, because `host/gereplay.c` currently writes
480x272 guest memory. Add a detail fixture with a sloped edge and sub-PSP-pixel
features that an enlarged 1x render cannot reproduce. Compare before/after a
full-frame render-to-texture pass to prove compositing retains that detail.
Repeated enhanced runs must be deterministic on the same driver/configuration.

Exercise 480x272, 960x544, 1920x1080, 2560x1440, 3440x1440, 3840x2160 and an
odd size such as 1365x767; include narrow windows, minimized/restore and high-DPI
monitor transitions. Cross aspect modes with resolution modes. Add synthetic
history for scratch format changes, masked stencil operations, guest CPU writes,
overlapping aliases, display/back-buffer selection and resize during a fade.
Fixed captures already contain camera state: resizing a replay does not rerun
Claude's guest camera replacement, so live aspect/culling tests are also needed.

The full `mission-effects.pad` replay covers history the snapshots cannot:
require all 87 events through poll 2210, zero bad accesses and stable game pacing.
Retain or improve the documented unsupported-state counts; distinguish the
existing reduced-bit-depth stencil limitation from regressions introduced here.

Historical 1x evidence in `docs/RENDER-CHECKS.md` reports mean GPU work of 0.98 ms,
p95 2.6 ms and maximum 3.38 ms. Those are old measurements, not a current forecast.
1080p has about 15.9 times the visible pixels of 480x272; 4K has about 63.5 times.
Two padded 1080p targets with RGBA8 plus depth24/stencil8 occupy about 33.8 MiB;
at 4K about 135 MiB, before copies, resolves and texture cache. Measure draw,
stencil, migration/resolve GPU time, readback CPU time and resident allocations
separately. Aim for GPU p95 below 16.7 ms on the recorded test hardware while
retaining the game's existing roughly 30 fps simulation cadence. Do not infer
4K performance by multiplying the old mean or by counting swap callbacks.

The first implementation task should be ownership/target tracing and a physical
GPU capture path, followed by the dimension split at 1x. That makes the risky
framebuffer assumptions measurable before higher-resolution output obscures them.

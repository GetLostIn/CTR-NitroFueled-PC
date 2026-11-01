# Frame generation (v0.3)

`bbport.ini` `framegen=off|fsr3` (`fsr`, `on` and `1` from test builds also mean `fsr3`;
`BB_FRAMEGEN` for scripts). In the launcher: Settings › Display › *Frame generation*; in game:
Insert › *Frame generation*. Off by default, applies live.

## What it does

AMD FSR 3.1.6 frame interpolation (optical flow + frame interpolation, the validated
`ffx-vulkan::fsr3-vk-framegeneration-3.1.6` slice of
[FireBurn/FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan), MIT) runs on every finished
frame and returns the image halfway between it and the previous one. The presenter shows that
generated frame first and the real frame half a game frame later, so the screen gets two evenly
spaced images per game frame. The game itself, its FPS limit, the 60 FPS patch and the guest
vblank are untouched: only what reaches the screen doubles.

```
game frames      N-1 ───────────────── N ───────────────── N+1
shown            N-1      G(N-1,N)     N      G(N,N+1)     N+1
                 |<-- T/2 -->|<-- T/2 -->|
```

It works on the presenter's finished frame (post-processing done, HUD included); the port's own
menu and FPS counter are drawn afterwards on every present, so they never smear. It works with
every upscaler and every internal resolution, in menus and in split screen:

- With FSR 3, FSR 4 or DLSS selected, the game's motion (the upscaler inputs pass: R32F depth
  and RG16F pixel motion, the same jitter and camera) is handed to the interpolation whenever it
  was written for this frame and is not larger than the frame (`game motion + optical flow`).
- Otherwise (FSR 1, off, menus without a 3D scene, a 4K scene shown in a smaller window) flat far
  depth and zero motion stand in and the optical flow carries the motion alone, like
  driver-level frame generation.

## How to get a smooth result

- **A 120 Hz or faster screen.** Frame generation doubles what is shown: at 144 Hz the ideal is
  the game at about 72 FPS (fps limit 72) and 144 images on screen. At 60 Hz it has nothing to
  gain.
- **Present mode.** With `Fifo` (V-Sync) the two presents go out one refresh apart by themselves
  and the game settles at half the refresh rate. With `Mailbox`/`Immediate` the presenter waits
  half a game frame between the generated and the real present (the smoothed frame time).
- The swapchain gets one more image while the setting is on at start (two presents per frame).

## Implementation

- `vk_ctr_framegen.{h,cpp}` (`CtrFrameGen`, owned by the presenter). One FSR 3.1.6 context per
  frame size (render size = frame size, so the game-motion and optical-flow modes share it and
  switching between them only resets the history, never rebuilds the context).
- `Presenter::PrepareFrame` → `GenerateFrame`: after the post-processing pass, on the draw
  scheduler (the recording threads are drained by `CommandBuffer()`):
  1. the frame (General, MUTABLE_FORMAT) is read raw through a UNORM view of its own byte
     order and copied to an RGBA8 image (`fg_convert.comp`), the only backbuffer format 3.1.6
     takes;
  2. prepare (optical flow + dilation) and dispatch (interpolation) into an RGBA8 image. The
     3.1.5 bridge leaves sampled imports in SHADER_READ_ONLY, so they are imported that way and
     moved there and back around the calls;
  3. the result is copied back into a per-presenter-frame output image in the frame's own
     format (BGRA swaps red and blue back), written through an RGBA8 storage view
     (MUTABLE_FORMAT + EXTENDED_USAGE). The presenter blits it exactly like the real frame.
- `Presenter::Present` → `PresentGenerated`: acquire, blit, the overlay, submit (waiting on the
  frame's draw submission), present; then the pacing wait (non-FIFO) and the real present as
  before. With the in-game menu open the generated frames are not shown.
- Frame ids start at 1 and are retired in order once the draw tick that recorded them has
  completed (`Scheduler::IsFree`), all on the GPU thread: no deferred callback touches the
  context from another thread.
- History resets: the first frame, a new frame size, a switch between game motion and optical
  flow, more than 100 ms between frames (loads, pauses, hitches) and frames it cannot take
  (10-bit HDR). A reset frame goes out alone.

### FSR-Vulkan patches (`gpu/patches/fsr-vulkan`)

- `0003`: the 3.1.6 wrapper tags the bridge frame on Prepare and retires its transient
  descriptor sets and constant buffers in RetireFrame (they leaked until out of memory).
- `0004`: the bridge stages its per-pass constant buffers into persistent 256 KiB mapped slabs
  instead of one `vkAllocateMemory` each. With frame generation on, the FSR 3 upscaler's
  context creation failed with "out of memory" a few minutes into a session (the 2026-10-16
  test logs: every run with a frame-generation context, never without one) and frame generation
  then reset its history on every frame. On Windows each allocation is a kernel call counted
  against the driver's allocation limit; dozens per frame were created and freed.

### Upscaler changes that came with it

- The FSR 3 upscaler context is created the first time FSR 3 itself runs (DLSS and FSR 4 never
  needed it, but a failed creation used to turn them off too). A failed creation now falls back
  to FSR 1 and is retried 600 frames later instead of staying off for the session.

## Log lines

```
FrameGen: FSR 3.1.6 context 1920x1080, 59.7 MB
FrameGen: generating at 1920x1080 (game motion + optical flow)
FrameGen: 72.0 game FPS -> 143.9 shown; totals: ... generated, ... not shown, ... history resets, ... with game motion (Fifo)
FrameGen: <reason>; off for this session
```

The status line comes every 10 s while the setting is on. *shown* should be about twice the
game rate; *history resets* should only grow on loads and size changes.

## What changed from the test builds of 2026-10-15/16

Two test lines were merged here:

- v1 (presenter-side, in `Present`) generated almost nothing: it waited for fresh game vectors
  that the presenter's own timing rarely saw, and `framegen=fsr3` in `bbport.ini` was read as
  off by its parser.
- v2 (flip-time, `CtrFsr3::GenerateFlip`) generated, but presented the real frame *before* the
  interpolated one (the generated image lies between the previous and the current frame, so the
  motion went back and forth), had no pacing (with Mailbox the second present replaced the first
  within microseconds), shared one output image between frames in flight, swizzled BGRA twice,
  and reset its history on every frame once the FSR 3 upscaler context failed (above).

v3 keeps v2's place in the frame and its layout fixes, and fixes the rest.

## Known limits

- The HUD is interpolated with the frame (fast HUD elements can shimmer); separating it would
  need the game's display pass to draw into two buffers.
- 8-bit frames only (HDR swapchains skip generation).
- DLSS Frame Generation is not wired (it needs RTX 40+, Streamline and Reflex).

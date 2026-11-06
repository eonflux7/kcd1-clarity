# Overlay input and drawing with a hidden shared presenter

## Observed failure

With the tested OptiScaler DX11-to-DX12 presentation path, F8/F3 stopped appearing
or accepting input even though Clarity loaded and upscaling worked. The DX11
swapchain belonged to a hidden presenter window. Its buffer was copied into a
shared texture before the hidden swapchain's `Present` hook ran.

Drawing the overlay at that later `Present` missed the visible copy. Attaching
input to the hidden window also missed the actual game-window messages.

## Scoped compatibility path

- When the render window is hidden, select the unique visible, unowned window
  titled `Kingdom Come: Deliverance` in the current process for overlay input.
  Other processes and ambiguous matches are not selected.
- Hook `ID3D11DeviceContext::CopyResource`. Draw before the copy only when the
  destination is a shared texture and the source has the same COM identity as
  one of the tracked swapchain's back buffers. Ordinary scene textures are not
  accepted just because they have matching dimensions.
- Limit the overlay draw to once between hidden Presents and guard against
  recursion. Keep the normal visible-window Present path unchanged.
- Release the source RTV on resize and when the source buffer changes; do not
  keep a separate back-buffer reference across `ResizeBuffers`.

No image capture, file upload, remote communication or game-rendering changes are
introduced by this overlay compatibility path.

## Validation and limits

The functional fix was manually checked on Steam 1.9.8, Windows, RTX 4060, at
2560x1440 borderless, with the tested OptiScaler shared presenter (reported
version 10.0.0.1). F8 opened, closed, and accepted mouse changes; native DLAA
remained correct. It was also checked with lower-resolution upscaling and the
existing frame-generation configuration. The game-tested build included the
sub-rect fixes from PR #1 at `804f442b6d8d8283fd311f07321b1a354f5d8d26`.

This contribution isolates the overlay changes against `main`; that isolated
integration is compiled separately. There is no automated end-to-end overlay
test. Other presenters, localised window titles, multiple visible game windows,
and exhaustive device-loss/resize scenarios remain unverified.

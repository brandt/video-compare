#pragma once

#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

extern "C" {
#include <libavutil/frame.h>
}

// Wraps libplacebo Vulkan state: instance, device, swapchain, renderer.
// Owns the Vulkan surface created through SDL.
class GpuRenderer {
 public:
  GpuRenderer();
  ~GpuRenderer();

  // Initialise Vulkan via SDL's surface and create the libplacebo pipeline.
  // Returns false on failure (falls back to SDL_Renderer in that case).
  bool init(SDL_Window* window);

  // Tear down all libplacebo / Vulkan resources.
  void destroy();

  // Upload an AVFrame to GPU textures for the given side (0=left, 1=right).
  // The frame can be in any pixel format supported by libplacebo (yuv420p,
  // yuv420p10le, nv12, etc.).  Returns false on failure.
  bool upload_frame(int side, const AVFrame* frame);

  // Release a previously mapped frame for the given side.
  void unmap_frame(int side);

  // A single side-rendering operation. `src` is in source (video pixel)
  // coordinates; `dst` is in swapchain (FBO pixel) coordinates.
  struct SideRenderOp {
    int side;
    float src_x0, src_y0, src_x1, src_y1;
    float dst_x0, dst_y0, dst_x1, dst_y1;
  };

  // A solid-colored filled-rect overlay, composited in sRGB space on top of
  // the rendered video. Destination is in swapchain (FBO pixel) coordinates.
  // Color channels are 0..1 (RGBA).
  struct OverlayOp {
    float dst_x0, dst_y0, dst_x1, dst_y1;
    float color[4];
  };

  // A text (or arbitrary RGBA image) overlay. The pixel data is uploaded
  // synchronously during `render()` to an internal per-slot `pl_tex` that
  // persists and is reused across frames (so pushing the same logical
  // overlay repeatedly avoids tex re-creation when dimensions match).
  //
  // `rgba_data` must remain valid through the `render()` call. After `render()`
  // returns, the caller may free its source surface.
  struct TextOverlayOp {
    const void* rgba_data;  // source pixels, RGBA8 (channel-order: R, G, B, A)
    int width, height;
    int stride;             // bytes per row
    float dst_x, dst_y;     // top-left in FBO coords
    float alpha;            // 0..1 multiplier on the texture alpha
  };

  // Render video frames + overlays + text to the swapchain. `ops` lists
  // per-side renders; `overlays` are monochrome filled-rect primitives;
  // `text_overlays` are RGBA bitmaps (e.g. pre-rendered text). The draw
  // order is: side renders → primitive overlays → text overlays.
  // `target_color` overrides target colorspace (e.g. for HDR passthrough).
  bool render(const SideRenderOp* ops, int num_ops,
              const OverlayOp* overlays, int num_overlays,
              const TextOverlayOp* text_overlays, int num_text_overlays,
              const struct pl_color_space* target_color);

  // Present the rendered frame.
  void present();

  // Resize swapchain (call after window resize).
  bool resize(int width, int height);

  // Access the underlying pl_gpu (for overlay texture creation).
  pl_gpu gpu() const { return vk_ ? vk_->gpu : nullptr; }

  // Upload an RGBA CPU surface as an overlay texture.
  // Returns a pl_tex that can be used as pl_overlay.tex.
  // The caller should NOT destroy this texture — it is managed by the
  // GpuRenderer (recreated as needed, destroyed on cleanup).
  pl_tex upload_overlay_tex(const uint8_t* rgba_data, int width, int height, int stride);

  bool is_initialized() const { return swapchain_ != nullptr; }

 private:
  pl_log log_;
  pl_vk_inst vk_inst_;
  pl_vulkan vk_;
  VkSurfaceKHR surface_;
  pl_swapchain swapchain_;
  pl_renderer renderer_;

  // Per-side frame state.  4 texture slots per side (for multi-plane formats).
  static constexpr int kSideCount = 2;
  pl_tex frame_tex_[kSideCount][4];
  struct pl_frame mapped_frames_[kSideCount];
  bool frame_mapped_[kSideCount];

  // Overlay texture (reused across frames).
  pl_tex overlay_tex_;

  // Single-pixel all-white texture, used as the source for monochrome
  // primitive overlays (split line, rects, dots). Lazy-created.
  pl_tex white_tex_;

  // Per-slot RGBA textures for text overlays. Grown as needed; slot i is
  // reused across frames so repeated same-sized uploads avoid tex recreate.
  std::vector<pl_tex> text_tex_slots_;

  SDL_Window* window_;
};

#pragma once

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

  // Render video frames + overlays to the swapchain. `ops` lists per-side
  // renders; `overlays` lists compositing rects drawn after all sides.
  // `target_color` overrides target colorspace (e.g. for HDR passthrough).
  bool render(const SideRenderOp* ops, int num_ops,
              const OverlayOp* overlays, int num_overlays,
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

  SDL_Window* window_;
};

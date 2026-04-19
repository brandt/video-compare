#include "display/gpu_renderer.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

// Tell the header we just want declarations, not inline definitions
// (those live in pl_libav_impl.c in this directory).
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

GpuRenderer::GpuRenderer()
    : log_(nullptr),
      vk_inst_(nullptr),
      vk_(nullptr),
      surface_(VK_NULL_HANDLE),
      swapchain_(nullptr),
      renderer_(nullptr),
      frame_tex_{},
      mapped_frames_{},
      frame_mapped_{},
      overlay_tex_(nullptr),
      white_tex_(nullptr),
      osd_capture_tex_(nullptr),
      window_(nullptr) {}

GpuRenderer::~GpuRenderer() {
  destroy();
}

bool GpuRenderer::init(SDL_Window* window) {
  window_ = window;

  // --- libplacebo logger ---
  struct pl_log_params log_p = pl_log_default_params;
  log_p.log_cb = pl_log_color;
  log_p.log_level = PL_LOG_WARN;
  log_ = pl_log_create(PL_API_VER, &log_p);
  if (!log_) {
    std::cerr << "GpuRenderer: failed to create pl_log" << std::endl;
    return false;
  }

  // --- Load Vulkan through SDL ---
  if (!SDL_Vulkan_LoadLibrary(nullptr)) {
    std::cerr << "GpuRenderer: SDL_Vulkan_LoadLibrary failed: " << SDL_GetError() << std::endl;
    return false;
  }

  PFN_vkGetInstanceProcAddr get_proc =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
  if (!get_proc) {
    std::cerr << "GpuRenderer: SDL_Vulkan_GetVkGetInstanceProcAddr failed" << std::endl;
    return false;
  }

  // --- Collect SDL-required instance extensions ---
  Uint32 ext_count = 0;
  const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
  if (!sdl_exts) {
    std::cerr << "GpuRenderer: SDL_Vulkan_GetInstanceExtensions failed: " << SDL_GetError() << std::endl;
    return false;
  }

  // --- Create VkInstance via libplacebo helper ---
  struct pl_vk_inst_params inst_p = pl_vk_inst_default_params;
  inst_p.get_proc_addr = get_proc;
  inst_p.extensions = sdl_exts;
  inst_p.num_extensions = static_cast<int>(ext_count);
  vk_inst_ = pl_vk_inst_create(log_, &inst_p);
  if (!vk_inst_) {
    std::cerr << "GpuRenderer: pl_vk_inst_create failed" << std::endl;
    return false;
  }

  // --- Create VkSurfaceKHR via SDL ---
  if (!SDL_Vulkan_CreateSurface(window_, vk_inst_->instance, nullptr, &surface_)) {
    std::cerr << "GpuRenderer: SDL_Vulkan_CreateSurface failed: " << SDL_GetError() << std::endl;
    return false;
  }

  // --- Create Vulkan device (libplacebo picks best GPU) ---
  struct pl_vulkan_params vk_p = pl_vulkan_default_params;
  vk_p.instance = vk_inst_->instance;
  vk_p.get_proc_addr = get_proc;
  vk_p.surface = surface_;
  vk_p.allow_software = false;
  vk_ = pl_vulkan_create(log_, &vk_p);
  if (!vk_) {
    std::cerr << "GpuRenderer: pl_vulkan_create failed" << std::endl;
    return false;
  }

  // --- Create swapchain ---
  struct pl_vulkan_swapchain_params sw_p = {};
  sw_p.surface = surface_;
  sw_p.present_mode = VK_PRESENT_MODE_FIFO_KHR;
  sw_p.swapchain_depth = 3;
  swapchain_ = pl_vulkan_create_swapchain(vk_, &sw_p);
  if (!swapchain_) {
    std::cerr << "GpuRenderer: pl_vulkan_create_swapchain failed" << std::endl;
    return false;
  }

  // Let the swapchain pick its initial size from the window.
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(window_, &w, &h);
  if (!pl_swapchain_resize(swapchain_, &w, &h)) {
    std::cerr << "GpuRenderer: initial pl_swapchain_resize failed" << std::endl;
    return false;
  }

  // --- Create high-level renderer ---
  renderer_ = pl_renderer_create(log_, vk_->gpu);
  if (!renderer_) {
    std::cerr << "GpuRenderer: pl_renderer_create failed" << std::endl;
    return false;
  }

  std::cerr << "GpuRenderer: Vulkan initialised successfully ("
            << w << "x" << h << ")" << std::endl;
  return true;
}

void GpuRenderer::destroy() {
  if (vk_ && vk_->gpu) {
    pl_gpu_finish(vk_->gpu);

    for (int s = 0; s < kSideCount; s++) {
      if (frame_mapped_[s]) {
        pl_unmap_avframe(vk_->gpu, &mapped_frames_[s]);
        frame_mapped_[s] = false;
      }
      for (int i = 0; i < 4; i++) {
        pl_tex_destroy(vk_->gpu, &frame_tex_[s][i]);
      }
    }
    pl_tex_destroy(vk_->gpu, &overlay_tex_);
    pl_tex_destroy(vk_->gpu, &white_tex_);
    pl_tex_destroy(vk_->gpu, &osd_capture_tex_);
    for (auto& tex : text_tex_slots_) {
      pl_tex_destroy(vk_->gpu, &tex);
    }
    text_tex_slots_.clear();
  }

  pl_renderer_destroy(&renderer_);
  pl_swapchain_destroy(&swapchain_);
  pl_vulkan_destroy(&vk_);

  if (vk_inst_ && surface_ != VK_NULL_HANDLE) {
    PFN_vkDestroySurfaceKHR destroySurface =
        reinterpret_cast<PFN_vkDestroySurfaceKHR>(
            vk_inst_->get_proc_addr(vk_inst_->instance, "vkDestroySurfaceKHR"));
    if (destroySurface) {
      destroySurface(vk_inst_->instance, surface_, nullptr);
    }
    surface_ = VK_NULL_HANDLE;
  }

  pl_vk_inst_destroy(&vk_inst_);
  pl_log_destroy(&log_);

  window_ = nullptr;
}

bool GpuRenderer::upload_frame(int side, const AVFrame* frame) {
  if (!vk_ || !frame) return false;
  if (frame->width <= 0 || frame->height <= 0 || frame->data[0] == nullptr) return false;
  if (side < 0 || side >= kSideCount) return false;

  // Unmap any previously mapped frame for this side.
  if (frame_mapped_[side]) {
    pl_unmap_avframe(vk_->gpu, &mapped_frames_[side]);
    frame_mapped_[side] = false;
  }

  memset(&mapped_frames_[side], 0, sizeof(mapped_frames_[side]));

  struct pl_avframe_params map_p = {};
  map_p.frame = frame;
  map_p.tex = frame_tex_[side];
  map_p.map_dovi = false;

  bool ok = pl_map_avframe_ex(vk_->gpu, &mapped_frames_[side], &map_p);
  if (!ok) {
    std::cerr << "GpuRenderer: pl_map_avframe_ex failed for side " << side
              << " (format=" << frame->format << " " << frame->width << "x" << frame->height << ")" << std::endl;
    return false;
  }

  frame_mapped_[side] = true;
  upload_w_[side] = frame->width;
  upload_h_[side] = frame->height;
  return true;
}

void GpuRenderer::unmap_frame(int side) {
  if (frame_mapped_[side] && vk_ && vk_->gpu) {
    pl_unmap_avframe(vk_->gpu, &mapped_frames_[side]);
    frame_mapped_[side] = false;
  }
}

// Ensure the shared single-pixel white texture exists. Used as the source
// for monochrome primitive overlays (split line, filled rects, progress dots).
static bool ensure_white_tex(pl_gpu gpu, pl_tex* tex) {
  if (*tex) return true;

  pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_UNORM, 1, 8, 0, PL_FMT_CAP_SAMPLEABLE);
  if (!fmt) return false;

  struct pl_tex_params tp = {};
  tp.w = 1;
  tp.h = 1;
  tp.format = fmt;
  tp.sampleable = true;
  tp.host_writable = true;
  tp.debug_tag = PL_DEBUG_TAG;

  if (!pl_tex_recreate(gpu, tex, &tp)) return false;

  const uint8_t pixel = 0xFF;
  struct pl_tex_transfer_params xfer = {};
  xfer.tex = *tex;
  xfer.ptr = (void*)&pixel;
  return pl_tex_upload(gpu, &xfer);
}

void GpuRenderer::compose_frame(struct pl_frame& target, int fbo_w, int fbo_h,
                                 const SideRenderOp* ops, int num_ops,
                                 const OverlayOp* overlays, int num_overlays,
                                 const TextOverlayOp* text_overlays, int num_text_overlays) {
  struct pl_render_params params = pl_render_fast_params;
  params.color_map_params = &pl_color_map_default_params;
  params.background = PL_CLEAR_SKIP;
  params.border = PL_CLEAR_SKIP;

  // Build primitive overlay (monochrome, one pl_overlay with N parts sharing
  // the 1×1 white texture). Done up-front so we can skip it in side renders.
  std::vector<struct pl_overlay_part> primitive_parts;
  struct pl_overlay primitive_overlay = {};
  bool have_primitives = false;

  if (num_overlays > 0 && ensure_white_tex(vk_->gpu, &white_tex_)) {
    primitive_parts.reserve(num_overlays);
    for (int i = 0; i < num_overlays; ++i) {
      // libplacebo asserts non-zero dst extent; skip degenerate rects.
      if (overlays[i].dst_x1 <= overlays[i].dst_x0 ||
          overlays[i].dst_y1 <= overlays[i].dst_y0) {
        continue;
      }
      struct pl_overlay_part p = {};
      p.src.x0 = 0; p.src.y0 = 0; p.src.x1 = 1; p.src.y1 = 1;
      p.dst.x0 = overlays[i].dst_x0;
      p.dst.y0 = overlays[i].dst_y0;
      p.dst.x1 = overlays[i].dst_x1;
      p.dst.y1 = overlays[i].dst_y1;
      memcpy(p.color, overlays[i].color, sizeof(p.color));
      primitive_parts.push_back(p);
    }
    if (!primitive_parts.empty()) {
      primitive_overlay.tex = white_tex_;
      primitive_overlay.mode = PL_OVERLAY_MONOCHROME;
      primitive_overlay.coords = PL_OVERLAY_COORDS_DST_FRAME;
      primitive_overlay.repr = pl_color_repr_rgb;
      primitive_overlay.color = pl_color_space_srgb;
      primitive_overlay.parts = primitive_parts.data();
      primitive_overlay.num_parts = static_cast<int>(primitive_parts.size());
      have_primitives = true;
    }
  }

  // No overlays attached to target during side renders — we composite overlays
  // in a separate image=NULL pass below so they're not applied multiple times.
  target.overlays = nullptr;
  target.num_overlays = 0;

  for (int i = 0; i < num_ops; ++i) {
    const SideRenderOp& op = ops[i];
    if (op.side < 0 || op.side >= kSideCount) continue;
    if (!frame_mapped_[op.side]) continue;

    struct pl_frame image = mapped_frames_[op.side];
    image.crop.x0 = op.src_x0;
    image.crop.y0 = op.src_y0;
    image.crop.x1 = op.src_x1;
    image.crop.y1 = op.src_y1;

    target.crop.x0 = op.dst_x0;
    target.crop.y0 = op.dst_y0;
    target.crop.x1 = op.dst_x1;
    target.crop.y1 = op.dst_y1;

    pl_render_image(renderer_, &image, &target, &params);
  }

  // Upload text-overlay pixel data to per-slot textures and build pl_overlays.
  // Slot i is reused across frames: pl_tex_recreate is a no-op when dims match.
  std::vector<struct pl_overlay> text_overlays_list;
  std::vector<struct pl_overlay_part> text_parts_storage; // one part per overlay
  text_overlays_list.reserve(num_text_overlays);
  text_parts_storage.reserve(num_text_overlays); // stable pointers guaranteed

  pl_fmt rgba_fmt = pl_find_fmt(vk_->gpu, PL_FMT_UNORM, 4, 8, 0, PL_FMT_CAP_SAMPLEABLE);

  for (int i = 0; i < num_text_overlays; ++i) {
    const TextOverlayOp& t = text_overlays[i];
    if (!t.rgba_data || t.width <= 0 || t.height <= 0 || !rgba_fmt) continue;

    // Grow slots as needed.
    if (static_cast<int>(text_tex_slots_.size()) <= i) {
      text_tex_slots_.resize(i + 1, nullptr);
    }

    struct pl_tex_params tp = {};
    tp.w = t.width;
    tp.h = t.height;
    tp.format = rgba_fmt;
    tp.sampleable = true;
    tp.host_writable = true;
    tp.debug_tag = PL_DEBUG_TAG;

    if (!pl_tex_recreate(vk_->gpu, &text_tex_slots_[i], &tp)) continue;

    struct pl_tex_transfer_params xfer = {};
    xfer.tex = text_tex_slots_[i];
    xfer.row_pitch = static_cast<size_t>(t.stride);
    xfer.ptr = const_cast<void*>(t.rgba_data);
    if (!pl_tex_upload(vk_->gpu, &xfer)) continue;

    struct pl_overlay_part part = {};
    part.src.x0 = 0; part.src.y0 = 0;
    part.src.x1 = static_cast<float>(t.width);
    part.src.y1 = static_cast<float>(t.height);
    part.dst.x0 = t.dst_x;
    part.dst.y0 = t.dst_y;
    part.dst.x1 = t.dst_x + static_cast<float>(t.width);
    part.dst.y1 = t.dst_y + static_cast<float>(t.height);
    // For PL_OVERLAY_NORMAL only color[3] (alpha) multiplies into the texture.
    part.color[0] = 1.0f; part.color[1] = 1.0f; part.color[2] = 1.0f;
    part.color[3] = t.alpha;

    // reserve() above guarantees push_back doesn't invalidate pointers.
    text_parts_storage.push_back(part);
    const struct pl_overlay_part* part_ptr = &text_parts_storage.back();

    struct pl_overlay ov = {};
    ov.tex = text_tex_slots_[i];
    ov.mode = PL_OVERLAY_NORMAL;
    ov.coords = PL_OVERLAY_COORDS_DST_FRAME;
    ov.repr = pl_color_repr_rgb;
    ov.color = pl_color_space_srgb;
    ov.parts = part_ptr;
    ov.num_parts = 1;
    text_overlays_list.push_back(ov);
  }

  // Assemble combined overlay list: primitive first (background rects, split,
  // dots), then text on top.
  std::vector<struct pl_overlay> all_overlays;
  if (have_primitives) all_overlays.push_back(primitive_overlay);
  for (const auto& ov : text_overlays_list) all_overlays.push_back(ov);

  // Final overlay-only pass (image = NULL). pl_render_image still composites
  // target.overlays onto the target when the image is NULL.
  if (!all_overlays.empty()) {
    target.overlays = all_overlays.data();
    target.num_overlays = static_cast<int>(all_overlays.size());
    // Use target.crop = full target so overlays in DST_FRAME coords aren't
    // clipped by a previously set sub-crop.
    target.crop.x0 = 0;
    target.crop.y0 = 0;
    target.crop.x1 = static_cast<float>(fbo_w);
    target.crop.y1 = static_cast<float>(fbo_h);
    pl_render_image(renderer_, nullptr, &target, &params);
  }
}

bool GpuRenderer::render(const SideRenderOp* ops, int num_ops,
                          const OverlayOp* overlays, int num_overlays,
                          const TextOverlayOp* text_overlays, int num_text_overlays,
                          const struct pl_color_space* target_color) {
  if (!swapchain_ || !renderer_) return false;

  struct pl_swapchain_frame sw_frame;
  if (!pl_swapchain_start_frame(swapchain_, &sw_frame)) {
    return false;
  }

  struct pl_frame target;
  pl_frame_from_swapchain(&target, &sw_frame);

  if (target_color) {
    target.color = *target_color;
  }

  // Clear target to background.
  const float bg[3] = {54.0f / 255.0f, 69.0f / 255.0f, 79.0f / 255.0f};
  pl_frame_clear(vk_->gpu, &target, bg);

  compose_frame(target, sw_frame.fbo->params.w, sw_frame.fbo->params.h,
                ops, num_ops, overlays, num_overlays,
                text_overlays, num_text_overlays);

  if (!pl_swapchain_submit_frame(swapchain_)) {
    std::cerr << "GpuRenderer: pl_swapchain_submit_frame failed" << std::endl;
    return false;
  }

  return true;
}

bool GpuRenderer::capture_osd(uint8_t* out_rgb24, int out_pitch, int width, int height,
                               const SideRenderOp* ops, int num_ops,
                               const OverlayOp* overlays, int num_overlays,
                               const TextOverlayOp* text_overlays, int num_text_overlays) {
  if (!vk_ || !renderer_ || !out_rgb24 || width <= 0 || height <= 0) return false;

  // Find a RGBA8 format that supports renderable + host-readable. Not every
  // Vulkan driver exposes a 3-channel renderable format, so capture via RGBA
  // and strip alpha on CPU.
  pl_fmt fmt = pl_find_fmt(vk_->gpu, PL_FMT_UNORM, 4, 8, 0,
                            static_cast<pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE |
                                                      PL_FMT_CAP_HOST_READABLE | PL_FMT_CAP_BLITTABLE));
  if (!fmt) {
    std::cerr << "GpuRenderer: no RGBA8 format with renderable+host_readable" << std::endl;
    return false;
  }

  struct pl_tex_params tp = {};
  tp.w = width;
  tp.h = height;
  tp.format = fmt;
  tp.renderable = true;
  tp.host_readable = true;
  tp.blit_dst = true;  // required by pl_frame_clear (blit-based)
  tp.sampleable = true;  // pl_render_image may sample the target when compositing
  tp.debug_tag = PL_DEBUG_TAG;
  if (!pl_tex_recreate(vk_->gpu, &osd_capture_tex_, &tp)) {
    std::cerr << "GpuRenderer: pl_tex_recreate (osd_capture_tex_) failed" << std::endl;
    return false;
  }

  // Build a sRGB pl_frame target on the capture tex. RGBA mapping mirrors
  // what pl_frame_from_swapchain would produce for an RGBA swapchain.
  struct pl_frame target = {};
  target.num_planes = 1;
  target.planes[0].texture = osd_capture_tex_;
  target.planes[0].components = 4;
  target.planes[0].component_mapping[0] = PL_CHANNEL_R;
  target.planes[0].component_mapping[1] = PL_CHANNEL_G;
  target.planes[0].component_mapping[2] = PL_CHANNEL_B;
  target.planes[0].component_mapping[3] = PL_CHANNEL_A;
  target.repr = pl_color_repr_rgb;
  target.color = pl_color_space_srgb;
  target.crop.x0 = 0;
  target.crop.y0 = 0;
  target.crop.x1 = static_cast<float>(width);
  target.crop.y1 = static_cast<float>(height);

  // Clear to the same background as render(), then composite the scene.
  const float bg[3] = {54.0f / 255.0f, 69.0f / 255.0f, 79.0f / 255.0f};
  pl_frame_clear(vk_->gpu, &target, bg);

  compose_frame(target, width, height,
                ops, num_ops, overlays, num_overlays,
                text_overlays, num_text_overlays);

  // Download as RGBA8 into a temporary buffer, then pack to RGB24 into the
  // caller-provided buffer (dropping alpha).
  const size_t row_bytes = static_cast<size_t>(width) * 4;
  std::vector<uint8_t> rgba_buf(row_bytes * static_cast<size_t>(height));

  struct pl_tex_transfer_params xfer = {};
  xfer.tex = osd_capture_tex_;
  xfer.ptr = rgba_buf.data();
  xfer.row_pitch = row_bytes;
  if (!pl_tex_download(vk_->gpu, &xfer)) {
    std::cerr << "GpuRenderer: pl_tex_download (osd) failed" << std::endl;
    return false;
  }

  for (int y = 0; y < height; ++y) {
    const uint8_t* src = rgba_buf.data() + static_cast<size_t>(y) * row_bytes;
    uint8_t* dst = out_rgb24 + static_cast<size_t>(y) * out_pitch;
    for (int x = 0; x < width; ++x) {
      dst[x * 3 + 0] = src[x * 4 + 0];
      dst[x * 3 + 1] = src[x * 4 + 1];
      dst[x * 3 + 2] = src[x * 4 + 2];
    }
  }

  return true;
}

void GpuRenderer::present() {
  if (swapchain_) {
    pl_swapchain_swap_buffers(swapchain_);
  }
}

bool GpuRenderer::resize(int width, int height) {
  if (!swapchain_) return false;
  return pl_swapchain_resize(swapchain_, &width, &height);
}

pl_tex GpuRenderer::upload_overlay_tex(const uint8_t* rgba_data, int width, int height, int stride) {
  if (!vk_ || !vk_->gpu) return nullptr;

  struct pl_tex_params tp = {};
  tp.w = width;
  tp.h = height;
  tp.format = pl_find_fmt(vk_->gpu, PL_FMT_UNORM, 4, 8, 0, PL_FMT_CAP_SAMPLEABLE);
  tp.sampleable = true;
  tp.host_writable = true;
  tp.debug_tag = PL_DEBUG_TAG;

  if (!pl_tex_recreate(vk_->gpu, &overlay_tex_, &tp)) return nullptr;

  struct pl_tex_transfer_params xfer = {};
  xfer.tex = overlay_tex_;
  xfer.row_pitch = static_cast<size_t>(stride);
  xfer.ptr = const_cast<uint8_t*>(rgba_data);

  return pl_tex_upload(vk_->gpu, &xfer) ? overlay_tex_ : nullptr;
}

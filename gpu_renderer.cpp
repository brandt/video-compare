#include "gpu_renderer.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

// Tell the header we just want declarations, not inline definitions
// (those live in pl_libav_impl.c).
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
  return true;
}

void GpuRenderer::unmap_frame(int side) {
  if (frame_mapped_[side] && vk_ && vk_->gpu) {
    pl_unmap_avframe(vk_->gpu, &mapped_frames_[side]);
    frame_mapped_[side] = false;
  }
}

bool GpuRenderer::render(const SideRenderOp* ops, int num_ops,
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

  struct pl_render_params params = pl_render_fast_params;
  params.color_map_params = &pl_color_map_default_params;
  params.background = PL_CLEAR_SKIP;
  params.border = PL_CLEAR_SKIP;

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

  if (!pl_swapchain_submit_frame(swapchain_)) {
    std::cerr << "GpuRenderer: pl_swapchain_submit_frame failed" << std::endl;
    return false;
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

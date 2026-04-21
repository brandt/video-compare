#pragma once
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <vector>
#include "core/core_types.h"
#include "subsystems/difference_processor.h"
#include "display_types.h"
#include "display_utils.h"
#include "display/gpu_renderer.h"
#include "subsystems/image_saver.h"
#include "subsystems/metadata_panel.h"
#include "subsystems/overlay_manager.h"
#include "pixel_format_utils.h"
#include "subsystems/playback_controller.h"
#include "subsystems/rgb_frame_cache.h"
#include "core/concurrency/row_workers.h"
#include "analysis/scopes/scope_window.h"
#include "subsystems/selection_manager.h"
#include "subsystems/view_transform.h"
extern "C" {
#include <libavutil/frame.h>
}

struct SDL {
  SDL();
  ~SDL();
};

class Display {
 public:
  using Mode = DisplayMode;
  using Loop = DisplayLoop;
  using AspectLockMode = DisplayAspectLockMode;
  using AspectViewMode = DisplayAspectViewMode;
  using DiffMode = DisplayDiffMode;
  using PlayState = DisplayPlayState;

  std::string mode_to_string(const Mode& mode) {
    switch (mode) {
      case Mode::Split:
        return "split";
      case Mode::VStack:
        return "vstack";
      case Mode::HStack:
        return "hstack";
      default:
        return "unknown";
    }
  }

  std::string aspect_lock_mode_to_string(const AspectLockMode& mode) {
    switch (mode) {
      case AspectLockMode::Off:
        return "off";
      case AspectLockMode::Window:
        return "window";
      case AspectLockMode::Content:
        return "content";
      default:
        return "unknown";
    }
  }

  std::string aspect_view_mode_to_string(const AspectViewMode& mode) {
    switch (mode) {
      case AspectViewMode::Stretch:
        return "stretch";
      case AspectViewMode::Original:
        return "original";
      case AspectViewMode::Preset16x9:
        return "16:9";
      case AspectViewMode::Preset4x3:
        return "4:3";
      case AspectViewMode::Preset1x1:
        return "1:1";
      default:
        return "unknown";
    }
  }

 private:
  const int display_number_;
  Mode mode_;
  const bool fit_window_to_usable_bounds_;
  const bool high_dpi_allowed_;
  const AspectLockMode aspect_lock_mode_;
  AspectViewMode aspect_view_mode_;
  const bool use_10_bpc_;
  bool hdr_display_available_{false};
  float hdr_display_headroom_{1.0f};
  float hdr_content_headroom_{10.0f};
  bool hdr_passthrough_{false};
  bool hdr_state_changed_{false};

  // True when frames arrive as RGB48LE and need convert_to_packed_10_bpc before upload.
  // HDR passthrough uses X2RGB10LE (already packed 4 bytes/pixel) — no conversion needed.
  bool requires_10_bpc() const { return use_10_bpc_ && !hdr_passthrough_; }
  bool fast_input_alignment_;
  bool bilinear_texture_filtering_;
  int video_width_;
  int video_height_;
  const double duration_;

  // SDL renderer drawable dimensions in physical pixels.
  // Can differ from window_width_/window_height_ on high-DPI displays.
  int drawable_width_;
  int drawable_height_;
  // Full SDL window dimensions (container area, including any letterbox/pillarbox margins).
  int window_width_;
  int window_height_;
  // Video content viewport inside the window in *window coordinates*.
  // In regular windowed mode this normally matches the full window.
  // In fullscreen-like states this can be centered and smaller than the full
  // window to preserve content aspect ratio without stretching.
  SDL_Rect content_window_{0, 0, 0, 0};

  // Scale from window coordinates to drawable pixel coordinates.
  float drawable_to_window_width_factor_;
  float drawable_to_window_height_factor_;
  // Scale from content-window coordinates to video-layout coordinates.
  // (layout is split/vstack/hstack space, not per-side source coordinates)
  float video_to_window_width_factor_;
  float video_to_window_height_factor_;
  // UI text scale derived from drawable/window density.
  float font_scale_;
  // Stored window aspect ratio used by AspectLockMode::Window.
  float window_aspect_ratio_{1.0F};

  // Last size we programmatically forced (used to avoid resize feedback loops).
  std::array<int, 2> last_forced_window_size_{{-1, -1}};
  // Initial window size captured at startup (restore with Ctrl+W).
  std::array<int, 2> startup_window_size_{{-1, -1}};
  // User-saved window size snapshot (save: Ctrl+Shift+W, restore: Shift+W).
  std::array<int, 2> saved_window_size_{{-1, -1}};
  // Windowed size to restore when exiting fullscreen.
  std::array<int, 2> windowed_size_before_fullscreen_{{-1, -1}};

  bool quit_{false};
  PlaybackController playback_;
  bool swap_left_right_{false};
  bool show_left_{true};
  bool show_right_{true};
  bool show_hud_{true};
  bool start_in_fullscreen_{false};
  bool is_fullscreen_{false};
  bool pending_verbose_print_{false};
  bool print_mouse_position_and_color_{false};
  bool print_image_similarity_metrics_{false};
  bool mouse_is_inside_window_{false};
  bool show_fps_{true};
  float current_video_fps_{0.0f};
  float current_ui_fps_{0.0f};

  bool show_quality_metrics_{false};
  std::string last_psnr_{"n/a"};
  std::string last_ssim_{"n/a"};
  std::string last_vmaf_{"n/a"};
  int64_t last_vmaf_left_pts_{INT64_MIN};
  int64_t last_vmaf_right_pts_{INT64_MIN};

  // Scope windows toggle requests
  std::array<bool, ScopeWindow::kNumScopes> toggle_scope_window_requested_{{false, false, false}};

  SelectionManager selection_;
  ImageSaver image_saver_;
  MetadataPanel metadata_panel_;
  OverlayManager overlay_;

  bool input_received_{true};
  // Reported by VideoCompare each refresh. Drives the SEEK badge in the play-
  // state tracker — true while left/right PTS diverge during post-seek or
  // sync-adjust catch-up. Defaults to true (assume in sync).
  bool playback_in_sync_{true};
  // Frame-ring occupancy relative to current playback position. Rendered in
  // the HUD opposite the FPS counters when show_fps_ is on.
  int frame_buffer_before_{0};
  int frame_buffer_after_{0};
  int64_t previous_left_frame_pts_;
  int64_t previous_right_frame_pts_;
  std::string previous_left_frame_key_;
  std::string previous_right_frame_key_;
  bool timer_based_update_performed_;

  // Adapter that exposes Display's layout fields as the read-only interface
  // ViewTransform consumes. Will be replaced with a reference to WindowLayout
  // in phase 9.
  class LayoutAdapter : public ViewTransformLayout {
   public:
    explicit LayoutAdapter(const Display& d) : d_(d) {}
    SDL_Rect content_window() const override { return d_.content_window_; }
    float video_to_window_width_factor() const override { return d_.video_to_window_width_factor_; }
    float video_to_window_height_factor() const override { return d_.video_to_window_height_factor_; }
    int video_width() const override { return d_.video_width_; }
    int video_height() const override { return d_.video_height_; }
    DisplayMode mode() const override { return d_.mode_; }

   private:
    const Display& d_;
  };
  LayoutAdapter layout_adapter_;
  ViewTransform view_transform_;

  // Per-frame snapshot built once at the top of possibly_refresh so both
  // GPU/SDL paths consume identical derived state without recomputing it.
  struct RenderContext {
    const AVFrame* left_frame;
    const AVFrame* right_frame;
    bool has_updated_left_frame;
    bool has_updated_right_frame;
    bool compare_mode;
    ViewTransform::ZoomRect zoom_rect;
    float video_mouse_x;
    float video_texel_clamped_mouse_x;
    int split_x;
    int dst_zoomed_size;
    int dst_half_zoomed_size;
  };

  void render_frame_gpu(const RenderContext& ctx, const std::string& current_total_browsable);
  void render_frame_sdl(const RenderContext& ctx, const std::string& current_total_browsable);

  // GPU render sub-phases (called in order from render_frame_gpu).
  bool gpu_run_cpu_work(const RenderContext& ctx);  // returns have_rgb
  void gpu_upload_frames(const RenderContext& ctx, bool have_rgb);
  void gpu_build_main_video_ops(const RenderContext& ctx, std::vector<GpuRenderer::SideRenderOp>& ops);
  void gpu_build_zoom_magnifier_ops(const RenderContext& ctx,
                                     std::vector<GpuRenderer::SideRenderOp>& ops,
                                     float& zoom_left_slider_dx,
                                     float& zoom_right_slider_dx);
  // Returns a captured OSD AVFrame when a save was requested and CPU pixels
  // are available; otherwise nullptr (the expensive GPU readback is skipped).
  AVFramePtr gpu_capture_osd(bool have_rgb,
                              const std::vector<GpuRenderer::SideRenderOp>& ops,
                              const std::vector<GpuRenderer::OverlayOp>& overlays,
                              const std::vector<GpuRenderer::TextOverlayOp>& text_ops);
  void gpu_finalize_deferred(const RenderContext& ctx, bool have_rgb, AVFrame* osd_frame);

  // SDL render sub-phases (called in order from render_frame_sdl).
  void sdl_run_cpu_work(const RenderContext& ctx,
                         const std::array<uint8_t*, 3>& planes_left, const std::array<size_t, 3>& pitches_left,
                         const std::array<uint8_t*, 3>& planes_right, const std::array<size_t, 3>& pitches_right);
  void sdl_render_video_textures(const RenderContext& ctx,
                                  const std::array<uint8_t*, 3>& planes_left, const std::array<size_t, 3>& pitches_left,
                                  const std::array<uint8_t*, 3>& planes_right, const std::array<size_t, 3>& pitches_right);
  void sdl_render_zoom_magnifier(int mouse_drawable_x, int mouse_drawable_y, int dst_zoomed_size);
  void sdl_render_hud(const RenderContext& ctx, const std::string& current_total_browsable);
  void sdl_render_message_toast();
  void sdl_finalize_deferred(const AVFrame* left_frame, const AVFrame* right_frame);

  // Event dispatch (called by handle_event by event type).
  void handle_window_event(const SDL_Event& event);
  void handle_mouse_motion_event(const SDL_Event& event);
  void handle_mouse_button_event(const SDL_Event& event);
  void handle_wheel_event(const SDL_Event& event);
  void handle_key_down(const SDL_Event& event);
  void handle_key_up(const SDL_Event& event);

  // Key-down cascade: first-match-wins. Each returns true when it consumed the key.
  bool handle_right_video_index_shortcut(SDL_Keycode keycode, bool is_ctrl_down, bool is_shift_down);
  bool handle_crop_save_keys(SDL_Keycode keycode, bool is_shift_down);
  bool handle_scope_window_keys(SDL_Keycode keycode, bool is_shift_down);
  bool handle_window_size_keys(SDL_Keycode keycode, bool is_shift_down, bool is_ctrl_down, bool is_alt_down);
  bool handle_view_mode_keys(SDL_Keycode keycode, bool is_shift_down, bool is_ctrl_down);
  bool handle_zoom_pan_keys(SDL_Keycode keycode, bool is_shift_down);
  bool handle_playback_keys(SDL_Keycode keycode, float relative_seek_scale, float playback_speed_scale, bool is_shift_down, bool is_ctrl_down, bool is_alt_down);
  bool handle_diff_keys(SDL_Keycode keycode, bool is_shift_down);
  bool handle_misc_keys(SDL_Keycode keycode, SDL_Keymod keymod, bool is_shift_down);

  // Input helpers.
  void update_cursor_mode();
  bool is_clipboard_mod_pressed(SDL_Keymod keymod, bool is_ctrl_down) const;

  SDL sdl_;
  TTF_Font* small_font_{nullptr};
  TTF_Font* big_font_{nullptr};
  SDL_Cursor* normal_mode_cursor_;
  SDL_Cursor* pan_mode_cursor_;
  SDL_Cursor* selection_mode_cursor_;

  DifferenceProcessor diff_processor_;

  struct SideUIState {
    SDL_Texture* text_texture{nullptr};
    int text_width{0};
    int text_height{0};
    std::string file_stem;
  };
  std::array<SideUIState, 2> side_ui_;

  int border_extension_;
  int double_border_extension_;
  int line1_y_;
  int line2_y_;
  int middle_y_;
  int max_text_width_;

  SDL_Window* window_;
  SDL_Renderer* renderer_;  // nullptr when gpu_renderer_active_

  // libplacebo GPU renderer (Phase 1: video frames only, no HUD).
  GpuRenderer gpu_renderer_;
  bool gpu_renderer_active_{false};

  // Per-side video textures (each is video_width_ × video_height_).
  // Index 0 = left side, 1 = right side.
  static constexpr int kSideCount = 2;
  SDL_Texture* side_textures_linear_[kSideCount]{};
  SDL_Texture* side_textures_nn_[kSideCount]{};

  // GPU-mode RGB cache for features that still need CPU pixel access:
  // subtraction mode, per-pixel inspector, live PSNR/SSIM/VMAF. Lazily
  // populated via on-demand sws_scale from the native YUV frames; main
  // GPU pipeline stays YUV-only when these features are off.
  RgbFrameCache rgb_cache_;

  SDL_Event event_;
  float mouse_x_;
  float mouse_y_;
  float wheel_sensitivity_;

  Side displayed_left_side_{LEFT};
  Side displayed_right_side_{RIGHT};
  size_t num_right_videos_{1};
  size_t active_right_index_{0};
  std::string left_file_name_;
  std::string right_file_name_;
  std::string last_window_title_;

  // Thread pool for parallel processing
  RowWorkers row_workers_;

  void print_verbose_info();

  void rebuild_fonts();
  void rebuild_side_ui_textures();
  void clamp_overlay_offsets();
  bool detect_fullscreen_like_state() const;
  float compute_content_aspect_ratio() const;
  float compute_active_content_aspect_ratio() const;
  std::array<int, 2> compute_mode_switch_target_window_size() const;
  void update_content_window_layout();
  void apply_window_size_and_relayout(int target_w, int target_h, bool force_layout_refresh);
  void set_fullscreen(bool fullscreen);
  void resize_window_for_mode_switch();
  void update_hdr_display_state();
  void handle_window_resize(bool reset_forced_size_guard = false, bool force_layout_refresh = false);
  void recreate_video_textures_for_current_mode();

  // SDL-path OSD capture: reads back via SDL_RenderReadPixels, packs into
  // an AVFrame, then delegates to ImageSaver::save_frames_with_osd. The GPU
  // path builds the OSD via GpuRenderer::capture_osd and calls ImageSaver
  // directly.
  void save_image_frames_sdl(const AVFrame* left_frame, const AVFrame* right_frame);

  inline int static round(const float value) { return static_cast<int>(std::round(value)); }

  SDL_FRect video_rect_to_drawable_transform(const SDL_FRect& rect) const {
    const float width_scale = drawable_to_window_width_factor_ / video_to_window_width_factor_;
    const float height_scale = drawable_to_window_height_factor_ / video_to_window_height_factor_;
    const float x_offset = static_cast<float>(content_window_.x) * drawable_to_window_width_factor_;
    const float y_offset = static_cast<float>(content_window_.y) * drawable_to_window_height_factor_;

    return {x_offset + rect.x * width_scale, y_offset + rect.y * height_scale, rect.w * width_scale, rect.h * height_scale};
  }

  void render_text(int x, int y, SDL_Texture* texture, int texture_width, int texture_height, int border_extension, bool left_adjust);

  void render_progress_dots(const float position, const float progress, const bool is_top);

  SDL_Surface* render_text_with_fallback(const std::string& text);

  SDL_Texture* get_side_texture(int side) const;
  void update_side_texture(int side, const void* pixels, int pitch);

  int round_and_clamp(const float value);

  void render_quality_metrics_overlay();
  void refresh_display_side_mapping();
  void update_window_title_with_current_roi();

  void refresh_selection_end_from_mouse();
  void draw_selection_rect();
  void possibly_save_selected_area(const AVFrame* left_frame, const AVFrame* right_frame);
  void possibly_apply_crop();

  using ZoomRect = ViewTransform::ZoomRect;

 public:
  Display(const int display_number,
          const Mode mode,
          const bool verbose,
          const bool fit_window_to_usable_bounds,
          const bool high_dpi_allowed,
          const AspectLockMode aspect_lock_mode,
          const AspectViewMode aspect_view_mode,
          const bool use_10_bpc,
          const bool fast_input_alignment,
          const bool bilinear_texture_filtering,
          const std::tuple<int, int> window_size,
          const unsigned width,
          const unsigned height,
          const double duration,
          const float wheel_sensitivity,
          const bool start_in_subtraction_mode,
          const bool start_in_fullscreen,
          const std::string& left_file_name,
          const std::string& right_file_name);
  ~Display();

  // Reinitialize size-dependent rendering resources while preserving the SDL window.
  void reinitialize_video_dimensions(unsigned width, unsigned height);

  // Copy frame to display
  bool possibly_refresh(const AVFrame* left_frame, const AVFrame* right_frame, const std::string& current_total_browsable);

  // Set a pending message to be displayed
  void set_pending_message(const std::string& message);

  // Notify the user with a message
  void notify_user(const std::string& message);

  // Bring focus back to the main window (avoid scope windows stealing keyboard focus)
  void focus_main_window();

  // Input model:
  // - The main loop centrally pumps SDL events (single SDL_PollEvent loop).
  // - The main loop calls begin_input_frame(), then dispatches each event via handle_event().
  void begin_input_frame();
  void handle_event(const SDL_Event& event);

  // Mark that *some* input/event occurred this frame (even if consumed by a scope window),
  // so the display refresh logic can behave the same as when Display used to poll events itself.
  void mark_input_received();

  bool get_quit() const;
  bool get_play() const;
  /**
   * Derive the high-level playback state label from play/pause, loop mode,
   * and PTS-sync status. Used by the HUD badge and by external introspection
   * (e.g. the debug input socket). Priority: Seek > loop modes > Play > Pause.
   */
  PlayState get_play_state() const;
  /**
   * Map a PlayState to its human-readable label without decoration.
   * Returns "SEEK", "LOOP >", "LOOP <>", "PLAY", or "PAUSE".
   * Callers that want brackets (e.g. the HUD) must add them.
   */
  static const char* play_state_label(PlayState state);
  Loop get_buffer_play_loop_mode() const;
  void set_buffer_play_loop_mode(const Loop& mode);
  bool get_buffer_play_forward() const;
  void toggle_buffer_play_direction();
  bool get_fast_input_alignment() const;
  bool get_swap_left_right() const;

  // Resolve the underlying pipeline Side that the user conceptually means when
  // they press a "right video" input (`+`/`-`, shift-click on the timeline,
  // auto-align keys). Under swap the follower is the underlying LEFT pipeline
  // because LEFT is rendered on the visual right half of the window. Shared by
  // every swap-aware input path; see docs/planning/Swap-seek.md.
  Side follower_side_for_input() const;
  float get_seek_relative() const;
  bool get_seek_from_start() const;
  int get_frame_buffer_offset_delta() const;
  int get_frame_navigation_delta() const;
  int get_shift_right_frames() const;
  bool get_auto_align_requested() const;
  AutoAlignMode get_auto_align_mode() const;
  bool get_right_only_seek() const;

  // Side that actually participates in the pending right-only seek. Meaningful
  // only when `get_right_only_seek()` is true. Normally RIGHT; becomes LEFT
  // when the user shift-clicked while `swap_left_right_` was active — shift-
  // click follows the visual position, not the underlying pipeline identity.
  Side get_right_only_seek_follower() const;
  float compute_frame_ssim(const AVFrame* left_frame, const AVFrame* right_frame);
  float get_playback_speed_factor() const;
  bool get_tick_playback() const;
  bool get_possibly_tick_playback() const;
  bool get_show_fps() const;
  void set_current_fps(float video_fps, float ui_fps) { current_video_fps_ = video_fps; current_ui_fps_ = ui_fps; }

  void update_metadata(const VideoMetadata left_metadata, const VideoMetadata right_metadata);
  void update_right_video(const std::string& right_file_name, const VideoMetadata right_metadata);

  std::pair<SDL_Rect, SDL_Rect> get_visible_rois_in_single_frame_coordinates() const;
  SDL_Rect get_visible_roi_in_single_frame_coordinates() const;

  bool get_gpu_renderer_active() const { return gpu_renderer_active_; }

  /** SDL window size in logical (point) coordinates. */
  int get_window_width() const { return window_width_; }
  /** SDL window size in logical (point) coordinates. */
  int get_window_height() const { return window_height_; }
  /** Renderer drawable size in physical pixels (may differ from window size on high-DPI displays). */
  int get_drawable_width() const { return drawable_width_; }
  /** Renderer drawable size in physical pixels (may differ from window size on high-DPI displays). */
  int get_drawable_height() const { return drawable_height_; }
  /** Raw SDL_Window handle for the main video-compare window. Used by external
   *  automation (e.g. the debug input socket) to call SDL_WarpMouseInWindow
   *  so injected mouse events route correctly. */
  SDL_Window* get_main_window() const { return window_; }

  // Upload a native-format (decoded/filtered) AVFrame for GPU rendering.
  // Must be called before possibly_refresh() when gpu_renderer_active_.
  void upload_native_frame(int side, const AVFrame* frame);

  bool get_hdr_display_available() const;
  float get_hdr_display_headroom() const;
  void set_hdr_passthrough(bool enabled);
  void set_hdr_content_headroom(float headroom);
  bool consume_hdr_state_change();

  bool get_toggle_scope_window_requested(const ScopeWindow::Type type) const;

  PendingCropRequest get_and_clear_pending_crop_request();

  // Multiple right video support
  void set_num_right_videos(const size_t num_right_videos);
  size_t get_num_right_videos() const;
  size_t get_active_right_index() const;
  void set_active_right_index(size_t index);

  // Frame-ring occupancy around the current playback position (for the HUD
  // counter next to the FPS readout). `before` is history, `after` is prefetch.
  void set_frame_buffer_counts(int before, int after) {
    frame_buffer_before_ = before;
    frame_buffer_after_ = after;
  }

  // Left/right PTS sync state (pushed by VideoCompare). Drives the SEEK badge.
  void set_playback_in_sync(bool in_sync) { playback_in_sync_ = in_sync; }
};

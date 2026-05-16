#pragma once
#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "core/core_types.h"

// User triage decision per input video. Default is Skip; the user cycles
// through Keep/Skip/Toss via the dock's tri-state control. Emitted as JSON
// on exit (see VideoCompare::format_results_json).
enum class DockAction : uint8_t {
  Skip = 0,
  Keep = 1,
  Toss = 2,
};

// A small RGBA8 bitmap. Used for thumbnails (filled by the async loader),
// the L/R badges, and the tri-state glyphs. An empty `rgba` means "not
// yet decoded" — render paths paint a black placeholder in its slot.
struct DockBitmap {
  std::vector<uint8_t> rgba;  // tightly packed, width*height*4
  int width{0};
  int height{0};

  bool empty() const { return rgba.empty() || width <= 0 || height <= 0; }
};

// One row in the dock — the union of metadata (Side + path) and UI state
// (action). The thumbnail itself lives separately so it can be mutated by
// the loader thread under thumb_mutex_ while the rest of the entry stays
// touched only by the main thread.
struct DockEntry {
  Side pipeline_side;       // LEFT or Side::Right(i); identifies the source pipeline
  std::string file_path;    // shown in tooltips / printed in results
  DockAction action{DockAction::Skip};
};

// Hideable bottom-of-window thumbnail picker. Owns visibility, per-entry
// triage state, and the cached thumbnail bitmaps. Layout is computed on
// demand from the current drawable size; render paths consume the cached
// rects and the cached bitmaps.
//
// Thread-safety: thumbnail bytes are written from a background loader
// thread (see ThumbnailLoader) and read from the main render thread.
// Everything else is main-thread only.
class Dock {
 public:
  Dock();
  ~Dock();

  Dock(const Dock&) = delete;
  Dock& operator=(const Dock&) = delete;

  // One-time initialization. `entries` are in CLI input order (left first,
  // then right_videos[0..N-1]); this order is preserved through the lifetime
  // of the dock and matches the result-output order.
  void init(std::vector<DockEntry> entries);

  // --- Visibility ---
  bool visible() const { return visible_; }
  void set_visible(bool v) { visible_ = v; }
  void toggle() { visible_ = !visible_; }

  // --- Layout ---
  // Recompute dock_rect_drawable_, per-thumb rects, and tri-state rects
  // for the given drawable size. Cheap; can be called on every window resize.
  // Returns the dock-bar height in drawable pixels (0 if no entries).
  int layout(int drawable_w, int drawable_h);

  // The dock-bar height in drawable pixels for the current layout
  // (0 if dock is hidden / not yet laid out).
  int dock_height_drawable() const { return visible_ ? dock_height_drawable_ : 0; }

  // Hit-test. Coordinates are in *drawable* pixels (i.e. event_->button.x
  // scaled by drawable_to_window_width_factor_ inversion).
  // hit_thumb returns the entry index (0..N) or -1 for "no thumb hit".
  // hit_tristate returns (entry_index, action) or (-1, Skip) for "no hit".
  int hit_thumb(int drawable_x, int drawable_y) const;
  std::pair<int, DockAction> hit_tristate(int drawable_x, int drawable_y) const;

  // True iff (x, y) is anywhere inside the dock bar (used to gate event
  // routing so video-area handlers don't see dock clicks).
  bool contains(int drawable_x, int drawable_y) const;

  // --- Mutators (main thread) ---
  void set_action(int entry_index, DockAction a);
  DockAction get_action(int entry_index) const;

  // Hover tracking: which thumb (if any) is currently under the mouse. Used
  // by the render path to draw a confirmation outline over the on-stage
  // video region when the hovered thumb is the one feeding a visual slot.
  // Pass -1 to clear.
  void set_hover_thumb(int entry_index) { hover_thumb_idx_ = entry_index; }
  int hover_thumb_index() const { return hover_thumb_idx_; }

  // Which entry indices currently feed visual-left / visual-right. -1 means
  // "no thumb is showing this badge" (e.g. before initial setup). Updated by
  // Display whenever the slot mapping changes.
  void set_slot_indices(int left_slot_entry_idx, int right_slot_entry_idx);
  int left_slot_index() const { return left_slot_idx_; }
  int right_slot_index() const { return right_slot_idx_; }

  // --- Thumbnails (thread-safe) ---
  // Called from the ThumbnailLoader worker thread when a thumbnail finishes
  // decoding. Locks thumb_mutex_; stores the bitmap; flips dirty_ so the
  // render path picks it up next frame.
  void set_thumbnail(int entry_index, DockBitmap bitmap);

  // Render-path snapshot: if `entry_index`'s thumbnail is dirty, returns the
  // bitmap (still owned by the dock; returned by reference-style copy) and
  // clears the dirty flag so subsequent renders just reuse the cached GPU
  // texture. If not dirty, returns an empty bitmap.
  DockBitmap take_dirty_thumbnail(int entry_index);

  // Whether *any* thumbnail bytes have been delivered for this entry yet.
  // The render path uses this to choose between "draw cached texture" and
  // "draw black placeholder".
  bool has_thumbnail(int entry_index) const;

  // --- Read-only accessors ---
  const std::vector<DockEntry>& entries() const { return entries_; }

  // Layout rects in *drawable* pixel coordinates. Valid only after layout().
  SDL_Rect dock_rect_drawable() const { return dock_rect_; }
  SDL_Rect well_rect_drawable(int i) const { return wells_[i]; }
  SDL_Rect thumb_rect_drawable(int i) const { return thumb_rects_[i]; }
  SDL_Rect tristate_rect_drawable(int i, DockAction a) const {
    return tristate_rects_[i][static_cast<size_t>(a)];
  }

  // Procedurally-generated badge bitmaps (magenta circle with white 'L',
  // blue circle with white 'R'). Lazily filled on first layout(); kept
  // until destruction. Exposed so render paths can upload them as overlay
  // textures.
  const DockBitmap& l_badge() const { return l_badge_; }
  const DockBitmap& r_badge() const { return r_badge_; }

  // Procedurally-generated tri-state glyph bitmaps (square aspect; the
  // render path scales them into tristate_rect_drawable cells).
  const DockBitmap& keep_glyph() const { return keep_glyph_; }
  const DockBitmap& skip_glyph() const { return skip_glyph_; }
  const DockBitmap& toss_glyph() const { return toss_glyph_; }

  // Returns the per-thumb target box size that the loader should aim for.
  // Computed from a representative drawable height; the loader pre-sizes its
  // thumbnails so the first layout fits cleanly. Stable once init() runs.
  int thumb_target_max_width() const { return thumb_target_max_w_; }
  int thumb_target_max_height() const { return thumb_target_max_h_; }

  // Snapshot of (path, action) per entry in CLI order. Used by the exit-time
  // results emitter.
  struct EntryResult {
    std::string path;
    DockAction action;
  };
  std::vector<EntryResult> snapshot_results() const;

 private:
  // Build the lazy procedural bitmaps (L/R badges, ✓/◯/✕ glyphs) once.
  void build_static_bitmaps();

  bool visible_{true};
  std::vector<DockEntry> entries_;

  // Layout outputs, in drawable pixels. wells_[i] is the per-entry box
  // (background); thumb_rects_[i] is the thumbnail image rect inside it;
  // tristate_rects_[i][action] is each tri-state icon's bbox.
  SDL_Rect dock_rect_{0, 0, 0, 0};
  int dock_height_drawable_{0};
  std::vector<SDL_Rect> wells_;
  std::vector<SDL_Rect> thumb_rects_;
  std::vector<std::array<SDL_Rect, 3>> tristate_rects_;

  // Suggested target for the async thumbnail loader.
  int thumb_target_max_w_{160};
  int thumb_target_max_h_{90};

  int left_slot_idx_{-1};
  int right_slot_idx_{-1};
  int hover_thumb_idx_{-1};

  // Procedural bitmaps (built once on first layout()).
  bool static_bitmaps_built_{false};
  DockBitmap l_badge_;
  DockBitmap r_badge_;
  DockBitmap keep_glyph_;
  DockBitmap skip_glyph_;
  DockBitmap toss_glyph_;

  // Thumbnails. Guarded by thumb_mutex_ — written by the loader thread,
  // read on the main thread.
  mutable std::mutex thumb_mutex_;
  std::vector<DockBitmap> thumbnails_;
  std::vector<bool> thumb_dirty_;
  std::vector<bool> thumb_has_;  // sticky once the first non-empty bitmap arrives
};

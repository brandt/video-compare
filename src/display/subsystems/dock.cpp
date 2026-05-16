#include "display/subsystems/dock.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// Pack one RGBA8 pixel into a DockBitmap at (x, y).
inline void put_pixel(DockBitmap& b, int x, int y, uint8_t r, uint8_t g, uint8_t bl, uint8_t a) {
  if (x < 0 || y < 0 || x >= b.width || y >= b.height) {
    return;
  }
  uint8_t* p = b.rgba.data() + (static_cast<size_t>(y) * b.width + x) * 4;
  p[0] = r;
  p[1] = g;
  p[2] = bl;
  p[3] = a;
}

inline void clear_bitmap(DockBitmap& b, uint8_t r, uint8_t g, uint8_t bl, uint8_t a) {
  for (int y = 0; y < b.height; ++y) {
    for (int x = 0; x < b.width; ++x) {
      put_pixel(b, x, y, r, g, bl, a);
    }
  }
}

// Filled antialiased circle (1-pixel edge softening).
void draw_filled_circle(DockBitmap& b, float cx, float cy, float radius,
                        uint8_t r, uint8_t g, uint8_t bl, uint8_t a) {
  const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius - 1)));
  const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius - 1)));
  const int x1 = std::min(b.width - 1, static_cast<int>(std::ceil(cx + radius + 1)));
  const int y1 = std::min(b.height - 1, static_cast<int>(std::ceil(cy + radius + 1)));
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      const float dx = static_cast<float>(x) + 0.5f - cx;
      const float dy = static_cast<float>(y) + 0.5f - cy;
      const float d = std::sqrt(dx * dx + dy * dy);
      if (d <= radius - 0.5f) {
        put_pixel(b, x, y, r, g, bl, a);
      } else if (d <= radius + 0.5f) {
        const float t = std::clamp(radius + 0.5f - d, 0.0f, 1.0f);
        const uint8_t aa = static_cast<uint8_t>(std::round(static_cast<float>(a) * t));
        // Source-over composite onto existing pixel (background is transparent
        // initially so this just sets the alpha-weighted color for the edge).
        put_pixel(b, x, y, r, g, bl, aa);
      }
    }
  }
}

// Ring outline (filled circle minus inner filled circle).
void draw_ring(DockBitmap& b, float cx, float cy, float r_outer, float r_inner,
               uint8_t r, uint8_t g, uint8_t bl, uint8_t a) {
  const int x0 = std::max(0, static_cast<int>(std::floor(cx - r_outer - 1)));
  const int y0 = std::max(0, static_cast<int>(std::floor(cy - r_outer - 1)));
  const int x1 = std::min(b.width - 1, static_cast<int>(std::ceil(cx + r_outer + 1)));
  const int y1 = std::min(b.height - 1, static_cast<int>(std::ceil(cy + r_outer + 1)));
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      const float dx = static_cast<float>(x) + 0.5f - cx;
      const float dy = static_cast<float>(y) + 0.5f - cy;
      const float d = std::sqrt(dx * dx + dy * dy);
      if (d >= r_inner - 0.5f && d <= r_outer + 0.5f) {
        float t = 1.0f;
        if (d < r_inner + 0.5f) {
          t = std::clamp(d - (r_inner - 0.5f), 0.0f, 1.0f);
        } else if (d > r_outer - 0.5f) {
          t = std::clamp(r_outer + 0.5f - d, 0.0f, 1.0f);
        }
        const uint8_t aa = static_cast<uint8_t>(std::round(static_cast<float>(a) * t));
        put_pixel(b, x, y, r, g, bl, aa);
      }
    }
  }
}

// Anti-aliased line segment with a given thickness, using Bresenham-ish
// rasterization that fills a per-pixel distance-to-segment buffer. Cheap and
// good enough for the chunky badge / tri-state glyphs.
void draw_thick_line(DockBitmap& b, float x0, float y0, float x1, float y1,
                     float thickness, uint8_t r, uint8_t g, uint8_t bl, uint8_t a) {
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  const float len_sq = dx * dx + dy * dy;
  const float half_t = thickness * 0.5f;
  const float half_t_plus = half_t + 0.5f;
  const int bx0 = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - half_t_plus)));
  const int by0 = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - half_t_plus)));
  const int bx1 = std::min(b.width - 1, static_cast<int>(std::ceil(std::max(x0, x1) + half_t_plus)));
  const int by1 = std::min(b.height - 1, static_cast<int>(std::ceil(std::max(y0, y1) + half_t_plus)));
  for (int y = by0; y <= by1; ++y) {
    for (int x = bx0; x <= bx1; ++x) {
      const float px = static_cast<float>(x) + 0.5f;
      const float py = static_cast<float>(y) + 0.5f;
      float t_seg = (len_sq > 1e-6f) ? ((px - x0) * dx + (py - y0) * dy) / len_sq : 0.0f;
      t_seg = std::clamp(t_seg, 0.0f, 1.0f);
      const float qx = x0 + t_seg * dx;
      const float qy = y0 + t_seg * dy;
      const float d = std::sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy));
      if (d <= half_t + 0.5f) {
        const float alpha_t = std::clamp(half_t + 0.5f - d, 0.0f, 1.0f);
        const uint8_t aa = static_cast<uint8_t>(std::round(static_cast<float>(a) * alpha_t));
        put_pixel(b, x, y, r, g, bl, aa);
      }
    }
  }
}

// Tiny built-in 5x7 glyphs for the L and R badges. Each row is a bitmask of
// 5 columns (bit 4 = leftmost). Drawn as filled rects scaled to the badge.
constexpr uint8_t kGlyphL[7] = {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111};
constexpr uint8_t kGlyphR[7] = {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001};

void stamp_glyph_5x7(DockBitmap& b, const uint8_t glyph[7],
                     float ox, float oy, float scale,
                     uint8_t r, uint8_t g, uint8_t bl, uint8_t a) {
  for (int gy = 0; gy < 7; ++gy) {
    for (int gx = 0; gx < 5; ++gx) {
      if ((glyph[gy] >> (4 - gx)) & 1) {
        const int x0 = static_cast<int>(std::round(ox + static_cast<float>(gx) * scale));
        const int y0 = static_cast<int>(std::round(oy + static_cast<float>(gy) * scale));
        const int x1 = static_cast<int>(std::round(ox + static_cast<float>(gx + 1) * scale));
        const int y1 = static_cast<int>(std::round(oy + static_cast<float>(gy + 1) * scale));
        for (int y = y0; y < y1; ++y) {
          for (int x = x0; x < x1; ++x) {
            put_pixel(b, x, y, r, g, bl, a);
          }
        }
      }
    }
  }
}

DockBitmap make_badge(char letter, uint8_t br, uint8_t bg, uint8_t bb) {
  DockBitmap b;
  b.width = 28;
  b.height = 28;
  b.rgba.assign(static_cast<size_t>(b.width) * b.height * 4, 0);
  // Filled circle.
  draw_filled_circle(b, 14.0f, 14.0f, 13.0f, br, bg, bb, 255);
  // White letter, ~14px tall (scale 2x for 5x7 → 10x14 effective glyph).
  const uint8_t* glyph = (letter == 'L') ? kGlyphL : kGlyphR;
  const float scale = 2.0f;
  const float glyph_w = 5.0f * scale;
  const float glyph_h = 7.0f * scale;
  const float ox = 14.0f - glyph_w * 0.5f;
  const float oy = 14.0f - glyph_h * 0.5f;
  stamp_glyph_5x7(b, glyph, ox, oy, scale, 255, 255, 255, 255);
  return b;
}

DockBitmap make_keep_glyph() {
  // Green checkmark on transparent background. Two thick line segments
  // forming a ✓. Drawn into a 24x24 buffer; the renderer scales as needed.
  DockBitmap b;
  b.width = 24;
  b.height = 24;
  b.rgba.assign(static_cast<size_t>(b.width) * b.height * 4, 0);
  const uint8_t gr = 60, gg = 200, gb = 70;
  draw_thick_line(b, 4.0f, 13.0f, 10.0f, 19.0f, 3.0f, gr, gg, gb, 255);
  draw_thick_line(b, 10.0f, 19.0f, 20.0f, 6.0f, 3.0f, gr, gg, gb, 255);
  return b;
}

DockBitmap make_skip_glyph() {
  // Gray empty ring.
  DockBitmap b;
  b.width = 24;
  b.height = 24;
  b.rgba.assign(static_cast<size_t>(b.width) * b.height * 4, 0);
  draw_ring(b, 12.0f, 12.0f, 8.5f, 6.5f, 180, 180, 180, 255);
  return b;
}

DockBitmap make_toss_glyph() {
  // Red X.
  DockBitmap b;
  b.width = 24;
  b.height = 24;
  b.rgba.assign(static_cast<size_t>(b.width) * b.height * 4, 0);
  const uint8_t rr = 220, rg = 60, rb = 60;
  draw_thick_line(b, 5.0f, 5.0f, 19.0f, 19.0f, 3.0f, rr, rg, rb, 255);
  draw_thick_line(b, 19.0f, 5.0f, 5.0f, 19.0f, 3.0f, rr, rg, rb, 255);
  return b;
}

}  // namespace

Dock::Dock() = default;
Dock::~Dock() = default;

void Dock::init(std::vector<DockEntry> entries) {
  entries_ = std::move(entries);
  const size_t n = entries_.size();
  wells_.assign(n, SDL_Rect{0, 0, 0, 0});
  thumb_rects_.assign(n, SDL_Rect{0, 0, 0, 0});
  tristate_rects_.assign(n, std::array<SDL_Rect, 3>{});
  focused_entry_idx_ = -1;
  {
    std::lock_guard<std::mutex> lock(thumb_mutex_);
    thumbnails_.assign(n, DockBitmap{});
    thumb_dirty_.assign(n, false);
    thumb_has_.assign(n, false);
  }
}

void Dock::build_static_bitmaps() {
  if (static_bitmaps_built_) {
    return;
  }
  // Magenta circle with white L; blue circle with white R.
  l_badge_ = make_badge('L', 230, 30, 200);
  r_badge_ = make_badge('R', 30, 110, 230);
  keep_glyph_ = make_keep_glyph();
  skip_glyph_ = make_skip_glyph();
  toss_glyph_ = make_toss_glyph();
  static_bitmaps_built_ = true;
}

int Dock::layout(int drawable_w, int drawable_h) {
  if (entries_.empty() || drawable_w <= 0 || drawable_h <= 0) {
    dock_rect_ = SDL_Rect{0, 0, 0, 0};
    dock_height_drawable_ = 0;
    return 0;
  }
  build_static_bitmaps();

  // Bar height: 18% of drawable_h, clamped to [110, 200] px (drawable px).
  const int bar_h = std::clamp(static_cast<int>(std::round(drawable_h * 0.18)), 110, 200);
  dock_height_drawable_ = bar_h;
  dock_rect_ = SDL_Rect{0, drawable_h - bar_h, drawable_w, bar_h};

  const int n = static_cast<int>(entries_.size());
  const int pad = 8;                       // gap between wells
  const int max_well_w = static_cast<int>(std::round(bar_h * 1.4));  // cap well width
  const int avail_w = drawable_w - pad;    // pad on left only; we add pad between each
  int well_w = (avail_w / n) - pad;
  if (well_w > max_well_w) {
    well_w = max_well_w;
  }
  if (well_w < 40) {
    well_w = 40;
  }
  const int total_used_w = n * well_w + (n + 1) * pad;
  const int x_start = (drawable_w - total_used_w) / 2 + pad;
  const int well_top = drawable_h - bar_h + 6;
  const int well_h = bar_h - 12;

  // Within each well: thumbnail on top, tri-state row on bottom, 4px gap.
  const int gap = 4;
  const int tri_row_h = std::clamp(well_h / 4, 18, 32);
  const int thumb_h = well_h - tri_row_h - gap * 2;
  const int thumb_w = well_w - gap * 2;

  // Suggested target box for the async loader, derived from this layout.
  thumb_target_max_w_ = std::max(64, thumb_w);
  thumb_target_max_h_ = std::max(36, thumb_h);

  for (int i = 0; i < n; ++i) {
    const int x = x_start + i * (well_w + pad);
    wells_[i] = SDL_Rect{x, well_top, well_w, well_h};
    thumb_rects_[i] = SDL_Rect{x + gap, well_top + gap, thumb_w, thumb_h};

    // Tri-state row: 3 equal cells across the well width.
    const int tri_y = well_top + well_h - tri_row_h - gap;
    const int tri_cell_w = (well_w - gap * 2) / 3;
    for (int a = 0; a < 3; ++a) {
      tristate_rects_[i][a] = SDL_Rect{
          x + gap + a * tri_cell_w,
          tri_y,
          tri_cell_w,
          tri_row_h,
      };
    }
  }
  return bar_h;
}

bool Dock::contains(int drawable_x, int drawable_y) const {
  if (!visible_) {
    return false;
  }
  return drawable_x >= dock_rect_.x && drawable_x < dock_rect_.x + dock_rect_.w &&
         drawable_y >= dock_rect_.y && drawable_y < dock_rect_.y + dock_rect_.h;
}

int Dock::hit_thumb(int drawable_x, int drawable_y) const {
  if (!visible_) {
    return -1;
  }
  for (size_t i = 0; i < thumb_rects_.size(); ++i) {
    const SDL_Rect& r = thumb_rects_[i];
    if (drawable_x >= r.x && drawable_x < r.x + r.w &&
        drawable_y >= r.y && drawable_y < r.y + r.h) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

std::pair<int, DockAction> Dock::hit_tristate(int drawable_x, int drawable_y) const {
  if (!visible_) {
    return {-1, DockAction::Skip};
  }
  for (size_t i = 0; i < tristate_rects_.size(); ++i) {
    for (int a = 0; a < 3; ++a) {
      const SDL_Rect& r = tristate_rects_[i][a];
      if (drawable_x >= r.x && drawable_x < r.x + r.w &&
          drawable_y >= r.y && drawable_y < r.y + r.h) {
        return {static_cast<int>(i), static_cast<DockAction>(a)};
      }
    }
  }
  return {-1, DockAction::Skip};
}

void Dock::set_action(int entry_index, DockAction a) {
  if (entry_index < 0 || entry_index >= static_cast<int>(entries_.size())) {
    return;
  }
  entries_[entry_index].action = a;
}

DockAction Dock::get_action(int entry_index) const {
  if (entry_index < 0 || entry_index >= static_cast<int>(entries_.size())) {
    return DockAction::Skip;
  }
  return entries_[entry_index].action;
}

void Dock::set_slot_indices(int left_slot_entry_idx, int right_slot_entry_idx) {
  left_slot_idx_ = left_slot_entry_idx;
  right_slot_idx_ = right_slot_entry_idx;
}

void Dock::set_focused_entry_index(int entry_index) {
  if (entry_index < 0 || entry_index >= static_cast<int>(entries_.size())) {
    focused_entry_idx_ = -1;
  } else {
    focused_entry_idx_ = entry_index;
  }
}

void Dock::set_thumbnail(int entry_index, DockBitmap bitmap) {
  std::lock_guard<std::mutex> lock(thumb_mutex_);
  if (entry_index < 0 || entry_index >= static_cast<int>(thumbnails_.size())) {
    return;
  }
  thumbnails_[entry_index] = std::move(bitmap);
  thumb_dirty_[entry_index] = true;
  if (!thumbnails_[entry_index].empty()) {
    thumb_has_[entry_index] = true;
  }
}

DockBitmap Dock::take_dirty_thumbnail(int entry_index) {
  std::lock_guard<std::mutex> lock(thumb_mutex_);
  if (entry_index < 0 || entry_index >= static_cast<int>(thumbnails_.size())) {
    return {};
  }
  if (!thumb_dirty_[entry_index]) {
    return {};
  }
  thumb_dirty_[entry_index] = false;
  // Return a copy so the render path can drop the lock immediately.
  return thumbnails_[entry_index];
}

bool Dock::has_thumbnail(int entry_index) const {
  std::lock_guard<std::mutex> lock(thumb_mutex_);
  if (entry_index < 0 || entry_index >= static_cast<int>(thumb_has_.size())) {
    return false;
  }
  return thumb_has_[entry_index];
}

std::vector<Dock::EntryResult> Dock::snapshot_results() const {
  std::vector<EntryResult> out;
  out.reserve(entries_.size());
  for (const auto& e : entries_) {
    out.push_back(EntryResult{e.file_path, e.action});
  }
  return out;
}

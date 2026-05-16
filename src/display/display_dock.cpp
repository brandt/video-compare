#include "display/display.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include "display/display_utils.h"
#include "display/gpu_renderer.h"

namespace {

// Build an SDL_Texture from a DockBitmap (RGBA8). Caller owns the texture.
SDL_Texture* texture_from_bitmap(SDL_Renderer* r, const DockBitmap& b) {
  if (r == nullptr || b.empty()) {
    return nullptr;
  }
  SDL_Surface* surface = SDL_CreateSurface(b.width, b.height, SDL_PIXELFORMAT_RGBA32);
  if (surface == nullptr) {
    return nullptr;
  }
  std::memcpy(surface->pixels, b.rgba.data(), static_cast<size_t>(b.width) * b.height * 4);
  SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surface);
  SDL_DestroySurface(surface);
  if (tex) {
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
  }
  return tex;
}

// Color helpers for the selected-action ring.
struct RGBA { uint8_t r, g, b, a; };
constexpr RGBA kKeepColor{60, 200, 70, 255};
constexpr RGBA kSkipColor{180, 180, 180, 255};
constexpr RGBA kTossColor{220, 60, 60, 255};
constexpr RGBA kFocusOutlineColor{255, 255, 255, 230};
constexpr int kFocusOutlineWidth = 2;

constexpr RGBA color_for_action(DockAction a) {
  switch (a) {
    case DockAction::Keep: return kKeepColor;
    case DockAction::Toss: return kTossColor;
    case DockAction::Skip: default: return kSkipColor;
  }
}

}  // namespace

void Display::destroy_dock_thumb_textures() {
  for (SDL_Texture* t : dock_thumb_textures_) {
    if (t) SDL_DestroyTexture(t);
  }
  dock_thumb_textures_.clear();
  dock_thumb_tex_valid_.clear();
}

// Compute the on-screen drawable-pixel region of the requested visual slot.
// In Split mode there is no fixed L/R partition (the split line follows the
// mouse), so we approximate by halving the content rect horizontally — good
// enough for the hover-confirmation outline. HStack uses the actual left/right
// halves; VStack uses top/bottom.
bool Display::slot_region_drawable(int slot, SDL_FRect& out) const {
  if (content_window_.w <= 0 || content_window_.h <= 0) {
    return false;
  }
  const float dx0 = static_cast<float>(content_window_.x) * drawable_to_window_width_factor_;
  const float dy0 = static_cast<float>(content_window_.y) * drawable_to_window_height_factor_;
  const float dx1 = dx0 + static_cast<float>(content_window_.w) * drawable_to_window_width_factor_;
  const float dy1 = dy0 + static_cast<float>(content_window_.h) * drawable_to_window_height_factor_;

  switch (mode_) {
    case Mode::HStack:
    case Mode::Split: {
      // Left half (slot 0) and right half (slot 1).
      const float mid = (dx0 + dx1) * 0.5f;
      if (slot == 0)      out = {dx0, dy0, mid - dx0, dy1 - dy0};
      else if (slot == 1) out = {mid, dy0, dx1 - mid, dy1 - dy0};
      else return false;
      return true;
    }
    case Mode::VStack: {
      const float mid = (dy0 + dy1) * 0.5f;
      if (slot == 0)      out = {dx0, dy0, dx1 - dx0, mid - dy0};
      else if (slot == 1) out = {dx0, mid, dx1 - dx0, dy1 - mid};
      else return false;
      return true;
    }
  }
  return false;
}

void Display::on_dock_visibility_changed() {
  // Layout depends on current drawable size; trigger a relayout pass and
  // force-refresh the content window so video area shrinks/grows immediately.
  if (dock_.visible()) {
    dock_.layout(drawable_width_, drawable_height_);
  }
  handle_window_resize(false, /*force_layout_refresh=*/true);
  input_received_ = true;
}

// ---------------------------------------------------------------------------
// SDL path

void Display::render_dock_sdl() {
  if (!dock_.visible()) {
    return;
  }
  const auto& entries = dock_.entries();
  if (entries.empty()) {
    return;
  }

  // Hover indicator: when the user hovers a thumb that's currently feeding a
  // visual slot, outline the on-stage video region for that slot so they get
  // a visual confirmation of which video is the L/R counterpart of that thumb.
  // Drawn underneath the dock bar so it doesn't extend behind the picker.
  const int hover_idx = dock_.hover_thumb_index();
  if (hover_idx >= 0) {
    int hover_slot = -1;
    if (hover_idx == dock_.left_slot_index())       hover_slot = 0;
    else if (hover_idx == dock_.right_slot_index()) hover_slot = 1;
    SDL_FRect slot_rect;
    if (hover_slot >= 0 && slot_region_drawable(hover_slot, slot_rect)) {
      const uint8_t r = (hover_slot == 0) ? 230 : 30;
      const uint8_t g = (hover_slot == 0) ? 30 : 110;
      const uint8_t b = (hover_slot == 0) ? 200 : 230;
      SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
      // Translucent fill (soft cue) + a 3-px solid outline (hard cue).
      SDL_SetRenderDrawColor(renderer_, r, g, b, 40);
      SDL_RenderFillRect(renderer_, &slot_rect);
      SDL_SetRenderDrawColor(renderer_, r, g, b, 255);
      for (int i = 0; i < 3; ++i) {
        SDL_FRect outline = {slot_rect.x + i, slot_rect.y + i,
                              slot_rect.w - 2 * i, slot_rect.h - 2 * i};
        if (outline.w <= 0 || outline.h <= 0) break;
        SDL_RenderRect(renderer_, &outline);
      }
    }
  }

  // Lazy-grow the per-entry texture caches to match dock size.
  if (dock_thumb_textures_.size() != entries.size()) {
    destroy_dock_thumb_textures();
    dock_thumb_textures_.assign(entries.size(), nullptr);
    dock_thumb_tex_valid_.assign(entries.size(), false);
  }

  // Bar background.
  const SDL_Rect bar = dock_.dock_rect_drawable();
  SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer_, 18, 18, 22, 220);
  SDL_FRect bar_frect = to_frect(bar);
  SDL_RenderFillRect(renderer_, &bar_frect);
  // Top hairline.
  SDL_SetRenderDrawColor(renderer_, 70, 70, 80, 255);
  SDL_RenderLine(renderer_, static_cast<float>(bar.x), static_cast<float>(bar.y) + 0.5f,
                 static_cast<float>(bar.x + bar.w), static_cast<float>(bar.y) + 0.5f);

  for (size_t i = 0; i < entries.size(); ++i) {
    const SDL_Rect well = dock_.well_rect_drawable(static_cast<int>(i));
    const SDL_Rect thumb_rect = dock_.thumb_rect_drawable(static_cast<int>(i));

    // Well background.
    SDL_SetRenderDrawColor(renderer_, 32, 32, 38, 255);
    SDL_FRect well_f = to_frect(well);
    SDL_RenderFillRect(renderer_, &well_f);

    // Pull new thumbnail bytes if the loader posted any.
    DockBitmap fresh = dock_.take_dirty_thumbnail(static_cast<int>(i));
    if (!fresh.empty()) {
      if (dock_thumb_textures_[i]) {
        SDL_DestroyTexture(dock_thumb_textures_[i]);
      }
      dock_thumb_textures_[i] = texture_from_bitmap(renderer_, fresh);
      dock_thumb_tex_valid_[i] = (dock_thumb_textures_[i] != nullptr);
    }

    SDL_FRect thumb_f = to_frect(thumb_rect);
    if (dock_thumb_tex_valid_[i] && dock_thumb_textures_[i] != nullptr) {
      SDL_RenderTexture(renderer_, dock_thumb_textures_[i], nullptr, &thumb_f);
    } else {
      // Black placeholder until the thumbnail arrives.
      SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
      SDL_RenderFillRect(renderer_, &thumb_f);
    }

    // L / R badge overlays in the top-left corner of the thumb when this
    // entry holds a slot.
    const bool is_left_slot = static_cast<int>(i) == dock_.left_slot_index();
    const bool is_right_slot = static_cast<int>(i) == dock_.right_slot_index();
    if (is_left_slot || is_right_slot) {
      const DockBitmap& badge = is_left_slot ? dock_.l_badge() : dock_.r_badge();
      SDL_Texture* badge_tex = texture_from_bitmap(renderer_, badge);
      if (badge_tex) {
        const float badge_size = std::min(28.0f, static_cast<float>(thumb_rect.h) * 0.4f);
        SDL_FRect dst{static_cast<float>(thumb_rect.x) + 4.0f,
                       static_cast<float>(thumb_rect.y) + 4.0f,
                       badge_size, badge_size};
        SDL_RenderTexture(renderer_, badge_tex, nullptr, &dst);
        SDL_DestroyTexture(badge_tex);
      }
    }

    // Tri-state row underneath the thumbnail.
    const DockAction current_action = dock_.get_action(static_cast<int>(i));
    for (int a = 0; a < 3; ++a) {
      const SDL_Rect cell = dock_.tristate_rect_drawable(static_cast<int>(i), static_cast<DockAction>(a));
      const DockBitmap& glyph =
          (a == static_cast<int>(DockAction::Keep)) ? dock_.keep_glyph()
        : (a == static_cast<int>(DockAction::Toss)) ? dock_.toss_glyph()
        : dock_.skip_glyph();
      SDL_Texture* glyph_tex = texture_from_bitmap(renderer_, glyph);
      if (!glyph_tex) continue;
      const bool selected = (static_cast<DockAction>(a) == current_action);
      SDL_SetTextureAlphaMod(glyph_tex, selected ? 255 : 110);
      const float pad = 3.0f;
      SDL_FRect dst{static_cast<float>(cell.x) + pad, static_cast<float>(cell.y) + pad,
                     static_cast<float>(cell.w) - 2 * pad, static_cast<float>(cell.h) - 2 * pad};
      SDL_RenderTexture(renderer_, glyph_tex, nullptr, &dst);
      SDL_DestroyTexture(glyph_tex);
      if (selected) {
        const RGBA c = color_for_action(static_cast<DockAction>(a));
        SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
        SDL_FRect outline = to_frect(cell);
        SDL_RenderRect(renderer_, &outline);
      }
    }
  }

  // Keyboard focus outline: thin white rectangle around the focused entry's
  // well. Drawn last so it sits above wells / thumbs / badges / tristate.
  const int focused = dock_.focused_entry_index();
  if (focused >= 0 && focused < static_cast<int>(entries.size())) {
    const SDL_Rect well = dock_.well_rect_drawable(focused);
    SDL_SetRenderDrawColor(renderer_, kFocusOutlineColor.r, kFocusOutlineColor.g,
                            kFocusOutlineColor.b, kFocusOutlineColor.a);
    for (int i = 0; i < kFocusOutlineWidth; ++i) {
      SDL_FRect r{static_cast<float>(well.x + i), static_cast<float>(well.y + i),
                  static_cast<float>(well.w - 2 * i), static_cast<float>(well.h - 2 * i)};
      SDL_RenderRect(renderer_, &r);
    }
  }
}

// ---------------------------------------------------------------------------
// GPU path

void Display::render_dock_gpu(std::vector<GpuRenderer::OverlayOp>& overlays,
                               std::vector<GpuRenderer::TextOverlayOp>& text_ops) {
  if (!dock_.visible()) {
    return;
  }
  const auto& entries = dock_.entries();
  if (entries.empty()) {
    return;
  }

  // Keep the bytes alive across the libplacebo render call. TextOverlayOp
  // references them by pointer; libplacebo copies into its internal pl_tex
  // during render(), so this just needs to outlive that call. The vector is
  // declared static for that lifetime — refreshed each frame.
  static thread_local std::vector<DockBitmap> kept_bitmaps;
  kept_bitmaps.clear();
  kept_bitmaps.reserve(entries.size());

  auto push_rect = [&](float x0, float y0, float x1, float y1, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    GpuRenderer::OverlayOp op;
    op.dst_x0 = x0; op.dst_y0 = y0;
    op.dst_x1 = x1; op.dst_y1 = y1;
    op.color[0] = r / 255.0f;
    op.color[1] = g / 255.0f;
    op.color[2] = b / 255.0f;
    op.color[3] = a / 255.0f;
    overlays.push_back(op);
  };

  // Hover indicator: outline the on-stage video region matching the slot of
  // the thumb under the mouse. Drawn before the dock bar so the bar can paint
  // over any portion of the outline that falls behind it.
  const int hover_idx = dock_.hover_thumb_index();
  if (hover_idx >= 0) {
    int hover_slot = -1;
    if (hover_idx == dock_.left_slot_index())       hover_slot = 0;
    else if (hover_idx == dock_.right_slot_index()) hover_slot = 1;
    SDL_FRect slot_rect;
    if (hover_slot >= 0 && slot_region_drawable(hover_slot, slot_rect)) {
      const uint8_t r = (hover_slot == 0) ? 230 : 30;
      const uint8_t g = (hover_slot == 0) ? 30 : 110;
      const uint8_t b = (hover_slot == 0) ? 200 : 230;
      // Translucent fill.
      push_rect(slot_rect.x, slot_rect.y, slot_rect.x + slot_rect.w, slot_rect.y + slot_rect.h, r, g, b, 40);
      // 3-pixel solid border (four thin rects).
      const float t = 3.0f;
      push_rect(slot_rect.x, slot_rect.y, slot_rect.x + slot_rect.w, slot_rect.y + t, r, g, b, 255);                          // top
      push_rect(slot_rect.x, slot_rect.y + slot_rect.h - t, slot_rect.x + slot_rect.w, slot_rect.y + slot_rect.h, r, g, b, 255); // bottom
      push_rect(slot_rect.x, slot_rect.y, slot_rect.x + t, slot_rect.y + slot_rect.h, r, g, b, 255);                          // left
      push_rect(slot_rect.x + slot_rect.w - t, slot_rect.y, slot_rect.x + slot_rect.w, slot_rect.y + slot_rect.h, r, g, b, 255); // right
    }
  }

  auto push_bitmap = [&](const DockBitmap& bm, const SDL_Rect& dst, float alpha) {
    if (bm.empty()) return;
    // Kept-bitmaps caches references the GPU op vector points to.
    kept_bitmaps.push_back(bm);
    const DockBitmap& src = kept_bitmaps.back();
    GpuRenderer::TextOverlayOp op;
    op.rgba_data = src.rgba.data();
    op.width = src.width;
    op.height = src.height;
    op.stride = src.width * 4;
    op.dst_x = static_cast<float>(dst.x);
    op.dst_y = static_cast<float>(dst.y);
    op.alpha = alpha;
    text_ops.push_back(op);
  };

  // Bar background.
  const SDL_Rect bar = dock_.dock_rect_drawable();
  push_rect(static_cast<float>(bar.x), static_cast<float>(bar.y),
            static_cast<float>(bar.x + bar.w), static_cast<float>(bar.y + bar.h),
            18, 18, 22, 220);
  // Top hairline.
  push_rect(static_cast<float>(bar.x), static_cast<float>(bar.y),
            static_cast<float>(bar.x + bar.w), static_cast<float>(bar.y) + 1.0f,
            70, 70, 80, 255);

  for (size_t i = 0; i < entries.size(); ++i) {
    const SDL_Rect well = dock_.well_rect_drawable(static_cast<int>(i));
    const SDL_Rect thumb_rect = dock_.thumb_rect_drawable(static_cast<int>(i));

    // Well background.
    push_rect(static_cast<float>(well.x), static_cast<float>(well.y),
              static_cast<float>(well.x + well.w), static_cast<float>(well.y + well.h),
              32, 32, 38, 255);

    // Thumbnail: pull dirty bytes, then push from the dock's stored copy.
    // For the GPU path we don't keep a side-car cache (libplacebo's
    // text_tex_slots_ already handles per-slot reuse) — every frame just
    // pushes the current bitmap by reference.
    DockBitmap fresh = dock_.take_dirty_thumbnail(static_cast<int>(i));
    // We need the bitmap visible every frame; track the latest version we've
    // seen per entry in a thread-local cache so we can keep pushing it.
    static thread_local std::vector<DockBitmap> per_entry_cache;
    if (per_entry_cache.size() != entries.size()) {
      per_entry_cache.assign(entries.size(), DockBitmap{});
    }
    if (!fresh.empty()) {
      per_entry_cache[i] = std::move(fresh);
    }
    if (!per_entry_cache[i].empty()) {
      // Scale-by-region: GpuRenderer::TextOverlayOp doesn't support arbitrary
      // dst rects, so the bitmap is rendered at its native size. We can't
      // resize on the GPU path here without extending GpuRenderer; instead,
      // sizing was set by the loader to match the thumb box, so it's already
      // close. Center within the thumb rect.
      const int dx = thumb_rect.x + std::max(0, (thumb_rect.w - per_entry_cache[i].width) / 2);
      const int dy = thumb_rect.y + std::max(0, (thumb_rect.h - per_entry_cache[i].height) / 2);
      SDL_Rect bm_dst{dx, dy, per_entry_cache[i].width, per_entry_cache[i].height};
      // Black placeholder under the thumb in case the bitmap is smaller than the box.
      push_rect(static_cast<float>(thumb_rect.x), static_cast<float>(thumb_rect.y),
                static_cast<float>(thumb_rect.x + thumb_rect.w),
                static_cast<float>(thumb_rect.y + thumb_rect.h),
                0, 0, 0, 255);
      push_bitmap(per_entry_cache[i], bm_dst, 1.0f);
    } else {
      // Black placeholder.
      push_rect(static_cast<float>(thumb_rect.x), static_cast<float>(thumb_rect.y),
                static_cast<float>(thumb_rect.x + thumb_rect.w),
                static_cast<float>(thumb_rect.y + thumb_rect.h),
                0, 0, 0, 255);
    }

    // L/R badge.
    const bool is_left_slot = static_cast<int>(i) == dock_.left_slot_index();
    const bool is_right_slot = static_cast<int>(i) == dock_.right_slot_index();
    if (is_left_slot || is_right_slot) {
      const DockBitmap& badge = is_left_slot ? dock_.l_badge() : dock_.r_badge();
      SDL_Rect dst{thumb_rect.x + 4, thumb_rect.y + 4, badge.width, badge.height};
      push_bitmap(badge, dst, 1.0f);
    }

    // Tri-state glyphs.
    const DockAction current_action = dock_.get_action(static_cast<int>(i));
    for (int a = 0; a < 3; ++a) {
      const SDL_Rect cell = dock_.tristate_rect_drawable(static_cast<int>(i), static_cast<DockAction>(a));
      const DockBitmap& glyph =
          (a == static_cast<int>(DockAction::Keep)) ? dock_.keep_glyph()
        : (a == static_cast<int>(DockAction::Toss)) ? dock_.toss_glyph()
        : dock_.skip_glyph();
      // Center glyph in cell.
      const int dx = cell.x + std::max(0, (cell.w - glyph.width) / 2);
      const int dy = cell.y + std::max(0, (cell.h - glyph.height) / 2);
      const bool selected = (static_cast<DockAction>(a) == current_action);
      SDL_Rect dst{dx, dy, glyph.width, glyph.height};
      push_bitmap(glyph, dst, selected ? 1.0f : 0.45f);
      if (selected) {
        const RGBA c = color_for_action(static_cast<DockAction>(a));
        // 4-side rect outline (1px) drawn as four thin filled rects.
        push_rect(static_cast<float>(cell.x), static_cast<float>(cell.y),
                  static_cast<float>(cell.x + cell.w), static_cast<float>(cell.y + 1), c.r, c.g, c.b, c.a);
        push_rect(static_cast<float>(cell.x), static_cast<float>(cell.y + cell.h - 1),
                  static_cast<float>(cell.x + cell.w), static_cast<float>(cell.y + cell.h), c.r, c.g, c.b, c.a);
        push_rect(static_cast<float>(cell.x), static_cast<float>(cell.y),
                  static_cast<float>(cell.x + 1), static_cast<float>(cell.y + cell.h), c.r, c.g, c.b, c.a);
        push_rect(static_cast<float>(cell.x + cell.w - 1), static_cast<float>(cell.y),
                  static_cast<float>(cell.x + cell.w), static_cast<float>(cell.y + cell.h), c.r, c.g, c.b, c.a);
      }
    }
  }

  // Keyboard focus outline: thin white rectangle around the focused well.
  // Pushed last so it sits above all earlier dock primitives.
  const int focused = dock_.focused_entry_index();
  if (focused >= 0 && focused < static_cast<int>(entries.size())) {
    const SDL_Rect well = dock_.well_rect_drawable(focused);
    const auto& c = kFocusOutlineColor;
    for (int i = 0; i < kFocusOutlineWidth; ++i) {
      const float x0 = static_cast<float>(well.x + i);
      const float y0 = static_cast<float>(well.y + i);
      const float x1 = static_cast<float>(well.x + well.w - i);
      const float y1 = static_cast<float>(well.y + well.h - i);
      push_rect(x0, y0,            x1,        y0 + 1, c.r, c.g, c.b, c.a);  // top
      push_rect(x0, y1 - 1,        x1,        y1,     c.r, c.g, c.b, c.a);  // bottom
      push_rect(x0, y0,            x0 + 1,    y1,     c.r, c.g, c.b, c.a);  // left
      push_rect(x1 - 1, y0,        x1,        y1,     c.r, c.g, c.b, c.a);  // right
    }
  }
}

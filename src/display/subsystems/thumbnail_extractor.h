#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include "display/subsystems/dock.h"

// Decode a single keyframe near the 10%-mark of `path` and downscale it into
// an RGBA8 DockBitmap that fits within (max_width, max_height) while
// preserving the source aspect ratio. On any failure (open / decode / sws),
// returns an empty DockBitmap and logs to stderr.
//
// This is a synchronous one-shot. It opens its own AVFormatContext and codec
// — it does NOT reuse the running playback pipelines. Software-decoder only;
// thumbnails are tiny and HW-accel setup costs more than it saves.
DockBitmap extract_thumbnail(const std::string& path, int max_width, int max_height);

// Owns one background worker thread that calls extract_thumbnail() for each
// path in order and invokes `on_ready(index, bitmap)` from the worker thread
// when each finishes. The callback must be thread-safe (Dock::set_thumbnail
// is — it acquires the dock's thumb_mutex_).
//
// Destruction signals cancellation and joins. extract_thumbnail() itself is
// not cancellable mid-call, so shutdown can pause briefly (~100ms typical)
// if a worker is mid-extraction.
class ThumbnailLoader {
 public:
  using ReadyCallback = std::function<void(size_t, DockBitmap)>;

  ThumbnailLoader(std::vector<std::string> paths,
                  int max_width, int max_height,
                  ReadyCallback on_ready);
  ~ThumbnailLoader();

  ThumbnailLoader(const ThumbnailLoader&) = delete;
  ThumbnailLoader& operator=(const ThumbnailLoader&) = delete;

 private:
  std::atomic_bool cancel_{false};
  std::thread worker_;
};

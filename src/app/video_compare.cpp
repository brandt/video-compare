#include "app/video_compare.h"
#include "analysis/metrics/metrics_calculator.h"
#include "app/debug_input_script.h"
#include "app/debug_input_socket.h"
#include <unordered_set>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>
#include <thread>
#include "core/ffmpeg/ffmpeg.h"
#include "analysis/scope_manager.h"
#include "analysis/scopes/scope_window.h"
#include "core/sdl_event_info.h"
#include "core/logging/side_aware_logger.h"
#include "core/data/sorted_flat_deque.h"
#include "core/strings/string_utils.h"
#include "media/video_filter_context.h"
extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

// Free a cached SwsContext owned by a SwsContextUniquePtr. Defined here rather
// than in the header so the header can forward-declare SwsContext and avoid
// pulling in libswscale.
void SwsContextDeleter::operator()(SwsContext* ctx) const noexcept {
  if (ctx != nullptr) {
    sws_freeContext(ctx);
  }
}

// Inter-stage queue depth between demuxer → decoder → filter → converter.
// Phase 3 shrank this from 5 to 3: now that the PacketRing is the main
// spill buffer, these queues just need enough headroom to smooth out
// burstiness across pipeline stages, not to cache seconds of content.
// Shrinking recoups frame memory (proportionally more at 4K HDR).
static constexpr size_t QUEUE_SIZE = 3;

static constexpr uint32_t SLEEP_PERIOD_MS = 10;

static constexpr uint32_t ONE_SECOND_US = 1000 * 1000;
static constexpr uint32_t RESYNC_UPDATE_RATE_US = ONE_SECOND_US / 10;
static constexpr uint32_t NOMINAL_FPS_UPDATE_RATE_US = 1 * ONE_SECOND_US;

static bool env_flag_enabled(const char* name) {
  const char* v = std::getenv(name);
  if (v == nullptr) {
    return false;
  }
  return (v[0] == '1') || (v[0] == 'y') || (v[0] == 'Y') || (v[0] == 't') || (v[0] == 'T');
}

static auto avpacket_deleter = [](AVPacket* packet) {
  av_packet_unref(packet);
  delete packet;
};

// For packets returned by av_packet_alloc / av_packet_clone (not `new`d).
static auto avpacket_free_deleter = [](AVPacket* packet) { av_packet_free(&packet); };

static auto avframe_deleter = [](AVFrame* frame) { av_frame_free(&frame); };

static auto avframe_and_data_deleter = [](AVFrame* frame) {
  av_freep(&frame->data[0]);
  avframe_deleter(frame);
};

static inline bool is_behind(int64_t frame1_pts, int64_t frame2_pts, int64_t delta_pts) {
  const float t1 = static_cast<float>(frame1_pts) * AV_TIME_TO_SEC;
  const float t2 = static_cast<float>(frame2_pts) * AV_TIME_TO_SEC;
  const float delta_s = static_cast<float>(delta_pts) * AV_TIME_TO_SEC - 1e-5F;

  const float diff = t1 - t2;
  const float tolerance = std::max(delta_s, 1.0F / 480.0F);

  return diff < -tolerance;
}

static inline int64_t compute_min_delta(const int64_t delta_left_pts, const int64_t delta_right_pts) {
  return std::min(delta_left_pts, delta_right_pts) * 8 / 10;
};

static inline bool is_in_sync(const int64_t left_pts, const int64_t right_pts, const int64_t delta_left_pts, const int64_t delta_right_pts) {
  const int64_t min_delta = compute_min_delta(delta_left_pts, delta_right_pts);

  return !is_behind(left_pts, right_pts, min_delta) && !is_behind(right_pts, left_pts, min_delta);
};

static inline int64_t compute_frame_delay(const int64_t left_pts, const int64_t right_pts) {
  return std::max(left_pts, right_pts);
}

static inline std::pair<size_t, size_t> calculate_max_dest_dimensions(const std::map<Side, std::unique_ptr<VideoFilterer>>& video_filterers) {
  size_t max_w = 0;
  size_t max_h = 0;

  for (const auto& pair : video_filterers) {
    max_w = std::max(max_w, pair.second->dest_width());
    max_h = std::max(max_h, pair.second->dest_height());
  }

  return {max_w, max_h};
}

static inline double calculate_shortest_duration_seconds(const std::map<Side, std::unique_ptr<Demuxer>>& demuxers) {
  double shortest = std::numeric_limits<double>::max();

  for (const auto& pair : demuxers) {
    shortest = std::min(shortest, pair.second->duration() * AV_TIME_TO_SEC);
  }

  return shortest;
}

static const int64_t NEAR_ZERO_TIME_SHIFT_THRESHOLD = static_cast<int64_t>(0.5 * MILLISEC_TO_AV_TIME);

static bool compare_av_dictionaries(AVDictionary* dict1, AVDictionary* dict2) {
  if (av_dict_count(dict1) != av_dict_count(dict2)) {
    return false;
  }

  AVDictionaryEntry* entry1 = nullptr;
  AVDictionaryEntry* entry2 = nullptr;

  while ((entry1 = av_dict_get(dict1, "", entry1, AV_DICT_IGNORE_SUFFIX))) {
    entry2 = av_dict_get(dict2, entry1->key, nullptr, 0);
    if (!entry2 || std::string(entry1->value) != std::string(entry2->value)) {
      return false;
    }
  }

  return true;
}

static bool produces_same_decoded_video(const VideoCompareConfig& config) {
  if (config.right_videos.empty()) {
    return false;
  }
  const auto matches_left_decode_source = [&](const InputVideo& right_video) {
    return (config.left.file_name == right_video.file_name) && (config.left.demuxer == right_video.demuxer) && (config.left.decoder == right_video.decoder) && (config.left.hw_accel_spec == right_video.hw_accel_spec) &&
           compare_av_dictionaries(config.left.demuxer_options, right_video.demuxer_options) && compare_av_dictionaries(config.left.decoder_options, right_video.decoder_options) &&
           compare_av_dictionaries(config.left.hw_accel_options, right_video.hw_accel_options);
  };

  return std::all_of(config.right_videos.begin(), config.right_videos.end(), matches_left_decode_source);
}

static inline AVPixelFormat determine_pixel_format(const VideoCompareConfig& config, const bool hdr_passthrough = false) {
  if (hdr_passthrough) {
    return AV_PIX_FMT_X2RGB10LE;
  }
  return config.use_10_bpc ? AV_PIX_FMT_RGB48LE : AV_PIX_FMT_RGB24;
}

static bool probe_hdr_display(const int display_number) {
  // Ensure SDL video is initialized so we can query display properties.
  // This is idempotent — SDL_Init can be called multiple times.
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    return false;
  }

  int num_displays = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&num_displays);
  if (displays == nullptr || num_displays == 0) {
    SDL_free(displays);
    return false;
  }

  const int index = (display_number >= 0 && display_number < num_displays) ? display_number : 0;
  SDL_PropertiesID props = SDL_GetDisplayProperties(displays[index]);
  SDL_free(displays);

  return props != 0 && SDL_GetBooleanProperty(props, SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
}

static inline int determine_sws_flags(const bool fast) {
  return fast ? SWS_FAST_BILINEAR : (SWS_BICUBIC | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND);
}

static inline bool use_fast_input_alignment(const VideoCompareConfig& config) {
  return config.fast_input_alignment;
}

static void sleep_for_ms(const uint32_t ms) {
  std::chrono::milliseconds sleep(ms);
  std::this_thread::sleep_for(sleep);
}

VideoCompare::~VideoCompare() = default;

VideoCompare::VideoCompare(const VideoCompareConfig& config)
    : config_(config), same_decoded_video_both_sides_(produces_same_decoded_video(config)), auto_loop_mode_(config.auto_loop_mode), frame_buffer_size_(config.frame_buffer_size), packet_buffer_bytes_(config.packet_buffer_bytes), time_shifter_(config.time_shift) {
  auto install_processor = [&](auto& processor_map, const ReadyToSeek::ProcessorThread thread, const Side& side, auto processor) {
    processor_map[side] = std::move(processor);
    ready_to_seek_.init(thread, side);
    // Default-construct the per-side seeking flag to false (idempotent)
    seeking_per_side_[side];
  };

  // Initialize all right videos
  if (config.right_videos.empty()) {
    throw std::logic_error{"At least one right video must be supplied"};
  }

  // Initialize left video demuxer and decoder
  install_processor(demuxers_, ReadyToSeek::ProcessorThread::Demultiplexer, LEFT, std::make_unique<Demuxer>(LEFT, config.left.demuxer, config.left.file_name, config.left.demuxer_options, config.left.decoder_options));
  install_processor(
      video_decoders_, ReadyToSeek::ProcessorThread::Decoder, LEFT,
      std::make_unique<VideoDecoder>(LEFT, config.left.decoder, config.left.hw_accel_spec, demuxers_[LEFT]->video_codec_parameters(), config.left.peak_luminance_nits, config.left.hw_accel_options, config.left.decoder_options));

  // Initialize all right video demuxers and decoders
  for (size_t i = 0; i < config.right_videos.size(); ++i) {
    const auto& right_config = config.right_videos[i];
    Side right_side = Side::Right(i);

    // Store file name in the unified map
    right_video_info_[right_side].file_name = right_config.file_name;

    install_processor(demuxers_, ReadyToSeek::ProcessorThread::Demultiplexer, right_side, std::make_unique<Demuxer>(right_side, right_config.demuxer, right_config.file_name, right_config.demuxer_options, right_config.decoder_options));
    install_processor(video_decoders_, ReadyToSeek::ProcessorThread::Decoder, right_side,
                      std::make_unique<VideoDecoder>(right_side, right_config.decoder, right_config.hw_accel_spec, demuxers_[right_side]->video_codec_parameters(), right_config.peak_luminance_nits, right_config.hw_accel_options,
                                                     right_config.decoder_options));
  }

  // Create VideoFilterContext to manage all videos for consistent auto-filter determination
  video_filter_context_.add(LEFT, demuxers_[LEFT].get(), video_decoders_[LEFT].get(), config.left.color_trc);

  for (size_t i = 0; i < config.right_videos.size(); ++i) {
    const auto& right_config = config.right_videos[i];
    Side right_side = Side::Right(i);
    video_filter_context_.add(right_side, demuxers_[right_side].get(), video_decoders_[right_side].get(), right_config.color_trc);
  }

  // Probe HDR display before constructing filterers — determines whether we can skip tonemapping.
  // HDR passthrough is only active when the display supports HDR AND all video sides have HDR content.
  // Mixed HDR+SDR comparisons fall back to CPU tonemap (existing behavior) because the shared
  // texture can only be tagged with one colorspace.
  const bool hdr_display = probe_hdr_display(config.display_number);

  if (hdr_display) {
    auto is_hdr_content = [](const VideoDecoder* decoder, const std::string& custom_trc) {
      return decoder->infer_dynamic_range(custom_trc) != DynamicRange::Standard;
    };

    bool all_hdr = is_hdr_content(video_decoders_[LEFT].get(), config.left.color_trc);
    for (size_t i = 0; i < config.right_videos.size() && all_hdr; ++i) {
      all_hdr = is_hdr_content(video_decoders_[Side::Right(i)].get(), config.right_videos[i].color_trc);
    }

    hdr_passthrough_active_ = all_hdr;

    if (all_hdr) {
      std::cerr << "HDR display detected; all content is HDR — using native passthrough." << std::endl;
    } else {
      std::cerr << "HDR display detected, but not all content is HDR — using CPU tonemap." << std::endl;
    }
  }

  // Initialize filterers using VideoFilterContext for consistent auto-filter determination.
  // gpu_color_processing = true: skip CPU tonemap/color conversion in the filter chain.
  // libplacebo's GPU renderer handles all color conversion. If GPU init fails,
  // FormatConverter's sws_scale provides a basic fallback.
  const bool gpu_color_processing = true;
  const AVPixelFormat output_pixel_format = gpu_color_processing ? AV_PIX_FMT_NONE : determine_pixel_format(config, hdr_passthrough_active_);
  const bool filterer_hdr_passthrough = gpu_color_processing ? false : hdr_passthrough_active_;

  install_processor(video_filterers_, ReadyToSeek::ProcessorThread::Filterer, LEFT,
                    std::make_unique<VideoFilterer>(LEFT, demuxers_[LEFT].get(), video_decoders_[LEFT].get(), config.left.tone_mapping_mode, config.left.boost_tone, config.left.video_filters, config.left.color_space,
                                                    config.left.color_range, config.left.color_primaries, config.left.color_trc, &video_filter_context_, config.disable_auto_filters, output_pixel_format, filterer_hdr_passthrough,
                                                    gpu_color_processing));

  // For each right video, use VideoFilterContext for auto-filter determination
  for (size_t i = 0; i < config.right_videos.size(); ++i) {
    const auto& right_config = config.right_videos[i];
    Side right_side = Side::Right(i);

    install_processor(video_filterers_, ReadyToSeek::ProcessorThread::Filterer, right_side,
                      std::make_unique<VideoFilterer>(right_side, demuxers_[right_side].get(), video_decoders_[right_side].get(), right_config.tone_mapping_mode, right_config.boost_tone, right_config.video_filters,
                                                      right_config.color_space, right_config.color_range, right_config.color_primaries, right_config.color_trc, &video_filter_context_, config.disable_auto_filters, output_pixel_format,
                                                      filterer_hdr_passthrough, gpu_color_processing));
  }

  // Calculate max dimensions from all videos
  {
    const auto dims = calculate_max_dest_dimensions(video_filterers_);
    max_width_ = dims.first;
    max_height_ = dims.second;
  }

  // Initialize format converters
  const bool initial_fast_input_alignment = use_fast_input_alignment(config_);
  recreate_format_converters(determine_sws_flags(initial_fast_input_alignment));

  // Calculate shortest duration
  shortest_duration_ = calculate_shortest_duration_seconds(demuxers_);

  timer_ = std::make_unique<Timer>();

  // Initialize queues for all videos
  for (const auto& pair : demuxers_) {
    const Side& side = pair.first;
    packet_queues_[side] = std::make_unique<PacketQueue>(QUEUE_SIZE);
    packet_rings_[side] = std::make_unique<PacketRing>(packet_buffer_bytes_, demuxers_[side]->time_base());
    decoded_frame_queues_[side] = std::make_shared<DecodedFrameQueue>(QUEUE_SIZE);
    filtered_frame_queues_[side] = std::make_unique<FrameQueue>(QUEUE_SIZE);
    converted_frame_queues_[side] = std::make_unique<FrameQueue>(QUEUE_SIZE);

    // Initialize media frame detection state
    auto& detection_state = media_frame_detection_states_[side];
    detection_state.cardinality.store(MediaFrameCardinality::Unknown, std::memory_order_relaxed);
    detection_state.decoded_count.store(0, std::memory_order_relaxed);
    detection_state.last_counted_pts.store(std::numeric_limits<int64_t>::min(), std::memory_order_relaxed);
  }
  auto dump_video_info = [&](const Side& side, const std::string& file_name) {
    const std::string dimensions = string_sprintf("%dx%d", video_decoders_[side]->width(), video_decoders_[side]->height());
    const std::string pixel_format_and_color_space =
        stringify_pixel_format(video_decoders_[side]->pixel_format(), video_decoders_[side]->color_range(), video_decoders_[side]->color_space(), video_decoders_[side]->color_primaries(), video_decoders_[side]->color_trc());

    std::string aspect_ratio;

    if (video_decoders_[side]->is_anamorphic(demuxers_[side].get())) {
      const AVRational display_aspect_ratio = video_decoders_[side]->display_aspect_ratio(demuxers_[side].get());
      aspect_ratio = string_sprintf(" [DAR %d:%d]", display_aspect_ratio.num, display_aspect_ratio.den);
    }

    // clang-format off
    auto info = string_sprintf(
      "Input: %9s%s, %s, %s, %s, %s, %s, %s, %s, %s, %s",
      dimensions.c_str(),
      aspect_ratio.c_str(),
      format_duration(demuxers_[side]->duration() * AV_TIME_TO_SEC).c_str(),
      stringify_frame_rate(demuxers_[side]->guess_frame_rate(), video_decoders_[side]->codec_context()->field_order).c_str(),
      stringify_decoder(video_decoders_[side].get()).c_str(),
      pixel_format_and_color_space.c_str(),
      demuxers_[side]->format_name().c_str(),
      file_name.c_str(),
      stringify_file_size(demuxers_[side]->file_size(), 2).c_str(),
      stringify_bit_rate(demuxers_[side]->bit_rate(), 1).c_str(),
      video_filterers_[side]->resolved_filter_description().c_str()
    );
    // clang-format on

    sa_log_info(side, info);
  };

  dump_video_info(LEFT, config.left.file_name.c_str());
  for (size_t i = 0; i < config.right_videos.size(); ++i) {
    Side right_side = Side::Right(i);
    dump_video_info(right_side, config.right_videos[i].file_name.c_str());
  }

  // Initialize metadata overlay
  auto collect_metadata = [&](const Side& side) -> VideoMetadata {
    VideoMetadata metadata;

    const std::string dimensions = string_sprintf("%dx%d", video_decoders_[side]->width(), video_decoders_[side]->height());
    metadata.set(MetadataProperties::RESOLUTION, dimensions);

    const AVRational sample_aspect_ratio = video_decoders_[side]->sample_aspect_ratio(demuxers_[side].get(), true);
    const AVRational display_aspect_ratio = video_decoders_[side]->display_aspect_ratio(demuxers_[side].get());

    if (sample_aspect_ratio.num > 0) {
      metadata.set(MetadataProperties::SAMPLE_ASPECT_RATIO, string_sprintf("%d:%d", sample_aspect_ratio.num, sample_aspect_ratio.den));
      metadata.set(MetadataProperties::DISPLAY_ASPECT_RATIO, string_sprintf("%d:%d", display_aspect_ratio.num, display_aspect_ratio.den));
    } else {
      metadata.set(MetadataProperties::SAMPLE_ASPECT_RATIO, "unknown");
      metadata.set(MetadataProperties::DISPLAY_ASPECT_RATIO, "unknown");
    }

    metadata.set(MetadataProperties::CODEC, video_decoders_[side]->codec()->name);
    metadata.set(MetadataProperties::FRAME_RATE, stringify_frame_rate_only(demuxers_[side]->guess_frame_rate()));
    metadata.set(MetadataProperties::FIELD_ORDER, stringify_field_order(video_decoders_[side]->codec_context()->field_order, "unknown"));
    metadata.set(MetadataProperties::DURATION, format_duration(demuxers_[side]->duration() * AV_TIME_TO_SEC));
    metadata.set(MetadataProperties::BIT_RATE, stringify_bit_rate(demuxers_[side]->bit_rate(), 1));
    metadata.set(MetadataProperties::FILE_SIZE, stringify_file_size(demuxers_[side]->file_size(), 2));
    metadata.set(MetadataProperties::CONTAINER, demuxers_[side]->format_name());
    metadata.set(MetadataProperties::PIXEL_FORMAT, av_get_pix_fmt_name(video_decoders_[side]->pixel_format()));
    metadata.set(MetadataProperties::COLOR_SPACE, av_color_space_name(video_decoders_[side]->color_space()));
    metadata.set(MetadataProperties::COLOR_PRIMARIES, av_color_primaries_name(video_decoders_[side]->color_primaries()));
    metadata.set(MetadataProperties::TRANSFER_CURVE, av_color_transfer_name(video_decoders_[side]->color_trc()));
    metadata.set(MetadataProperties::COLOR_RANGE, av_color_range_name(video_decoders_[side]->color_range()));
    metadata.set(MetadataProperties::HARDWARE_ACCELERATION, video_decoders_[side]->is_hw_accelerated() ? video_decoders_[side]->hw_accel_name() : "None");
    metadata.set(MetadataProperties::FILTERS, video_filterers_[side]->resolved_filter_description());

    return metadata;
  };

  for (size_t i = 0; i < config.right_videos.size(); ++i) {
    Side right_side = Side::Right(i);
    right_video_info_[right_side].metadata = collect_metadata(right_side);
  }

  left_video_metadata_ = collect_metadata(LEFT);

  // Sticky single-decoder mode: enabled iff same-file, multiplier 1:1, and -t offset
  // is below the near-zero threshold. Once disabled (by the first frame shift / scrub
  // / crop / filter change in the session), it never re-enables.
  single_decoder_mode_.init(same_decoded_video_both_sides_ && (av_q2d(time_shifter_.multiplier()) == 1.0) && (std::abs(time_shifter_.offset_av_time()) < NEAR_ZERO_TIME_SHIFT_THRESHOLD));

  const Side active_right = Side::Right(active_right_index_);
  const auto right_it = right_video_info_.find(active_right);
  const std::string right_file_name = (right_it != right_video_info_.end()) ? right_it->second.file_name : right_video_info_.begin()->second.file_name;

  display_ = std::make_unique<Display>(config_.display_number, config_.display_mode, config_.verbose, config_.fit_window_to_usable_bounds, config_.high_dpi_allowed, config_.aspect_lock_mode, config_.aspect_view_mode, config_.use_10_bpc,
                                       use_fast_input_alignment(config_), config_.bilinear_texture_filtering, config_.window_size, max_width_, max_height_, shortest_duration_, config_.wheel_sensitivity, config_.start_in_subtraction_mode,
                                       config_.start_in_fullscreen, config_.start_paused, config_.left.file_name, right_file_name);
  if (hdr_passthrough_active_) {
    // Set content headroom from peak luminance (max across all sides)
    unsigned max_peak_nits = 0;
    for (const auto& pair : video_decoders_) {
      const DynamicRange dr = pair.second->infer_dynamic_range("");
      const unsigned peak = pair.second->safe_peak_luminance_nits(dr);
      max_peak_nits = std::max(max_peak_nits, peak);
    }
    display_->set_hdr_content_headroom(static_cast<float>(max_peak_nits) / 100.0f);
  }
  display_->set_hdr_passthrough(hdr_passthrough_active_);
  display_->set_num_right_videos(right_video_info_.size());
  display_->set_active_right_index(active_right_index_);
  display_->update_metadata(left_video_metadata_, right_video_info_[active_right].metadata);

  scope_manager_ = std::make_unique<ScopeManager>(config.scopes, config.use_10_bpc, config.display_number);

  // Move focus to main window if any scope windows are enabled
  if (config.scopes.histogram || config.scopes.vectorscope || config.scopes.waveform) {
    display_->focus_main_window();
  }
}

void VideoCompare::recreate_format_converter_for_side(const Side& side, const int sws_flags) {
  // In GPU-renderer mode (libplacebo does color/format work at render time),
  // the format converter is used purely to rescale sub-max frames up to
  // max_width_ × max_height_ — preserving the filterer's native output format.
  // This keeps every uploaded texture at identical dims, avoiding fractional
  // src-crop artifacts at the split boundary. In SDL-renderer mode the
  // converter handles both format conversion and dim scaling as before. At
  // ctor time display_ is not yet constructed; assume GPU (matches the
  // filterer ctor assumption) and let the SDL-fallback rebuild correct it.
  const bool gpu_color = !display_ || display_->get_gpu_renderer_active();
  const auto& filterer = video_filterers_.at(side);
  const AVPixelFormat output_pixel_format = gpu_color ? filterer->dest_pixel_format() : determine_pixel_format(config_, hdr_passthrough_active_);

  ready_to_seek_.init(ReadyToSeek::ProcessorThread::Converter, side);
  format_converters_[side] = std::make_unique<FormatConverter>(filterer->dest_width(), filterer->dest_height(), max_width_, max_height_, filterer->dest_pixel_format(), output_pixel_format, video_decoders_[side]->color_space(),
                                                               video_decoders_[side]->color_range(), side, sws_flags);
}

void VideoCompare::recreate_format_converters(const int sws_flags) {
  format_converters_.clear();

  for (const auto& pair : video_filterers_) {
    recreate_format_converter_for_side(pair.first, sws_flags);
  }
}

bool VideoCompare::handle_hdr_state_change() {
  if (!display_->consume_hdr_state_change()) {
    return false;
  }

  // Determine new HDR passthrough state based on updated display capability + content type
  const bool hdr_display = display_->get_hdr_display_available();
  bool new_passthrough = false;

  if (hdr_display) {
    auto is_hdr_content = [](const VideoDecoder* decoder, const std::string& custom_trc) {
      return decoder->infer_dynamic_range(custom_trc) != DynamicRange::Standard;
    };

    new_passthrough = is_hdr_content(video_decoders_[LEFT].get(), config_.left.color_trc);
    for (size_t i = 0; i < config_.right_videos.size() && new_passthrough; ++i) {
      new_passthrough = is_hdr_content(video_decoders_[Side::Right(i)].get(), config_.right_videos[i].color_trc);
    }
  }

  if (new_passthrough == hdr_passthrough_active_) {
    return false;
  }

  hdr_passthrough_active_ = new_passthrough;
  std::cerr << "HDR state changed; " << (new_passthrough ? "enabling" : "disabling") << " HDR passthrough." << std::endl;

  // Reconstruct filterers with new HDR passthrough state
  const bool gpu_color = display_ && display_->get_gpu_renderer_active();
  const AVPixelFormat output_pixel_format = gpu_color ? AV_PIX_FMT_NONE : determine_pixel_format(config_, hdr_passthrough_active_);
  const bool filt_hdr_pt = gpu_color ? false : hdr_passthrough_active_;

  video_filterers_[LEFT] = std::make_unique<VideoFilterer>(LEFT, demuxers_[LEFT].get(), video_decoders_[LEFT].get(), config_.left.tone_mapping_mode, config_.left.boost_tone, config_.left.video_filters, config_.left.color_space,
                                                           config_.left.color_range, config_.left.color_primaries, config_.left.color_trc, &video_filter_context_, config_.disable_auto_filters, output_pixel_format, filt_hdr_pt,
                                                           gpu_color);

  for (size_t i = 0; i < config_.right_videos.size(); ++i) {
    const auto& right_config = config_.right_videos[i];
    Side right_side = Side::Right(i);

    video_filterers_[right_side] = std::make_unique<VideoFilterer>(right_side, demuxers_[right_side].get(), video_decoders_[right_side].get(), right_config.tone_mapping_mode, right_config.boost_tone, right_config.video_filters,
                                                                   right_config.color_space, right_config.color_range, right_config.color_primaries, right_config.color_trc, &video_filter_context_, config_.disable_auto_filters,
                                                                   output_pixel_format, filt_hdr_pt, gpu_color);
  }

  // Recalculate dimensions and recreate format converters
  const auto dims = calculate_max_dest_dimensions(video_filterers_);
  max_width_ = dims.first;
  max_height_ = dims.second;

  recreate_format_converters(determine_sws_flags(display_->get_fast_input_alignment()));

  // Update display textures
  if (hdr_passthrough_active_) {
    unsigned max_peak_nits = 0;
    for (const auto& pair : video_decoders_) {
      const DynamicRange dr = pair.second->infer_dynamic_range("");
      const unsigned peak = pair.second->safe_peak_luminance_nits(dr);
      max_peak_nits = std::max(max_peak_nits, peak);
    }
    display_->set_hdr_content_headroom(static_cast<float>(max_peak_nits) / 100.0f);
  }
  display_->set_hdr_passthrough(hdr_passthrough_active_);

  return true;
}

PlaybackStateSnapshot VideoCompare::get_playback_state_snapshot() const {
  std::lock_guard<std::mutex> lock(playback_state_snapshot_mutex_);
  return playback_state_snapshot_;
}

const std::string& VideoCompare::get_left_path() const {
  return config_.left.file_name;
}

std::string VideoCompare::get_active_right_path() const {
  const Side active_right = Side::Right(active_right_index_);
  const auto it = right_video_info_.find(active_right);
  if (it != right_video_info_.end()) {
    return it->second.file_name;
  }
  // Fallback: if the active index isn't populated for any reason, return the
  // first registered right video so callers always get a usable path.
  if (!right_video_info_.empty()) {
    return right_video_info_.begin()->second.file_name;
  }
  return {};
}

double VideoCompare::get_uptime_seconds() const {
  const auto now = std::chrono::steady_clock::now();
  const std::chrono::duration<double> delta = now - start_time_;
  return delta.count();
}

void VideoCompare::operator()() {
  // Launch all threads
  for (const auto& pair : demuxers_) {
    const Side& side = pair.first;

    stages_.emplace_back([this, side]() { demultiplex(side); });
    stages_.emplace_back([this, side]() { decode_video(side); });
    stages_.emplace_back([this, side]() { filter_video(side); });
    stages_.emplace_back([this, side]() { format_convert_video(side); });
  }

  // If VIDEO_COMPARE_INPUT_SCRIPT points to a script file, spawn a detached
  // thread that drives the SDL event queue per the script. See
  // app/debug_input_script.h for the format.
  debug_input_script::start_from_env();

  // If VIDEO_COMPARE_INPUT_SOCK is set, bring up an interactive JSON-lines
  // control server on that Unix domain socket. See app/debug_input_socket.h.
  debug_input_socket::start_from_env({this, display_.get()});

  compare();

  for (auto& stage : stages_) {
    stage.join();
  }

  exception_holder_.rethrow_stored_exception();
}

void VideoCompare::demultiplex(const Side& side) {
  ScopedLogSide scoped_log_side(side);

  try {
    while (keep_running()) {
      // Wait for decoder to drain
      if (is_seeking(side) && ready_to_seek_.get(ReadyToSeek::ProcessorThread::Decoder, side)) {
        ready_to_seek_.set(ReadyToSeek::ProcessorThread::Demultiplexer, side);

        sleep_for_ms(SLEEP_PERIOD_MS);
        continue;
      }
      // Sleep if we are finished for now
      if (packet_queues_[side]->is_stopped() || (side.is_right() && single_decoder_mode_.enabled())) {
        sleep_for_ms(SLEEP_PERIOD_MS);
        continue;
      }

      // Create AVPacket
      AVPacketUniquePtr packet{new AVPacket, avpacket_deleter};
      av_init_packet(packet.get());
      packet->data = nullptr;

      // Read frame into AVPacket
      if (!(*demuxers_[side])(*packet)) {
        // Enter wait state if EOF
        packet_queues_[side]->stop();
        continue;
      }

      // Move into queue if first video stream
      if (packet->stream_index == demuxers_[side]->video_stream_index()) {
        // Clone into the encoded-packet spill buffer for the L1 re-decode path.
        // Fails silently on OOM (av_packet_clone returns nullptr); the live
        // pipeline still gets the original.
        if (AVPacket* cloned = av_packet_clone(packet.get())) {
          packet_rings_[side]->append(PacketRing::PacketPtr(cloned, avpacket_free_deleter));
        }
        packet_queues_[side]->push(std::move(packet));
      }
    }
  } catch (...) {
    exception_holder_.store_current_exception();
    quit_all_queues();
  }
}

void VideoCompare::decode_video(const Side& side) {
  ScopedLogSide scoped_log_side(side);

  try {
    while (keep_running()) {
      // Sleep if we are finished for now
      if (decoded_frame_queues_[side]->is_stopped() || (side.is_right() && single_decoder_mode_.enabled())) {
        if (is_seeking(side)) {
          // Flush the decoder
          video_decoders_[side]->flush();

          // Seeks are now OK
          ready_to_seek_.set(ReadyToSeek::ProcessorThread::Decoder, side);
        }

        sleep_for_ms(SLEEP_PERIOD_MS);
        continue;
      }

      AVPacketUniquePtr packet{nullptr, avpacket_deleter};

      // Read packet from queue
      if (!packet_queues_[side]->pop(packet)) {
        // Flush remaining frames cached in the decoder
        while (process_packet(side, packet.get())) {
          ;
        }

        // Enter wait state
        decoded_frame_queues_[side]->stop();
        if (single_decoder_mode_.enabled()) {
          for (auto& pair : decoded_frame_queues_) {
            if (pair.first.is_right()) {
              pair.second->stop();
            }
          }
        }
        continue;
      }

      // If the packet didn't send, receive more frames and try again
      while (!is_seeking(side) && !process_packet(side, packet.get())) {
        ;
      }
    }
  } catch (...) {
    exception_holder_.store_current_exception();
    quit_all_queues();
  }
}

bool VideoCompare::process_packet(const Side& side, AVPacket* packet) {
  bool sent = video_decoders_[side]->send(packet);

  while (true) {
    AVFrameSharedPtr frame_decoded{av_frame_alloc(), avframe_deleter};

    // If a whole frame has been decoded, adjust time stamps and add to queue
    if (!video_decoders_[side]->receive(frame_decoded.get(), demuxers_[side].get())) {
      break;
    }

    AVFrameSharedPtr frame_for_filtering;

    if (frame_decoded->format == video_decoders_[side]->hw_pixel_format()) {
      AVFrameSharedPtr sw_frame_decoded{av_frame_alloc(), avframe_deleter};

      // Transfer data from GPU to CPU
      if (av_hwframe_transfer_data(sw_frame_decoded.get(), frame_decoded.get(), 0) < 0) {
        throw std::runtime_error("Error transferring frame from GPU to CPU");
      }
      if (av_frame_copy_props(sw_frame_decoded.get(), frame_decoded.get()) < 0) {
        throw std::runtime_error("Copying SW frame properties");
      }

      frame_for_filtering = sw_frame_decoded;
    } else {
      frame_for_filtering = frame_decoded;
    }

    if (!decoded_frame_queues_[side]->push(frame_for_filtering)) {
      return sent;
    }
    note_decoded_frame(side, frame_for_filtering->pts);

    // Send the decoded frame to all right filterers when a single decoder drives all sides.
    if (single_decoder_mode_.enabled() && side.is_left()) {
      for (auto& pair : decoded_frame_queues_) {
        if (pair.first.is_right()) {
          pair.second->push(frame_for_filtering);
          note_decoded_frame(pair.first, frame_for_filtering->pts);
        }
      }
    }
  }

  return sent;
}

void VideoCompare::filter_decoded_frame(const Side& side, AVFrameSharedPtr frame_decoded) {
  // send decoded frame to filterer
  if (!video_filterers_[side]->send(frame_decoded.get())) {
    throw std::runtime_error("Error while feeding the filter graph");
  }

  while (true) {
    AVFrameUniquePtr frame_filtered{av_frame_alloc(), avframe_deleter};

    // get next filtered frame
    if (!video_filterers_[side]->receive(frame_filtered.get())) {
      break;
    }

    if (!filtered_frame_queues_[side]->push(std::move(frame_filtered))) {
      return;
    }
  }

  return;
}

void VideoCompare::filter_video(const Side& side) {
  ScopedLogSide scoped_log_side(side);

  try {
    while (keep_running()) {
      if (filtered_frame_queues_[side]->is_stopped()) {
        if (is_seeking(side)) {
          ready_to_seek_.set(ReadyToSeek::ProcessorThread::Filterer, side);
        }

        sleep_for_ms(SLEEP_PERIOD_MS);
        continue;
      }

      AVFrameSharedPtr frame_to_filter;

      if (decoded_frame_queues_[side]->pop(frame_to_filter)) {
        filter_decoded_frame(side, frame_to_filter);
      } else if (decoded_frame_queues_[side]->is_stopped() || is_seeking(side)) {
        // Close the filter source
        video_filterers_[side]->close_src();

        // Flush the filter graph
        filter_decoded_frame(side, nullptr);

        // Stop filtering
        filtered_frame_queues_[side]->stop();
      }
    }
  } catch (...) {
    exception_holder_.store_current_exception();
    quit_all_queues();
  }
}

void VideoCompare::format_convert_video(const Side& side) {
  ScopedLogSide scoped_log_side(side);

  try {
    while (keep_running()) {
      if (converted_frame_queues_[side]->is_stopped()) {
        if (is_seeking(side)) {
          ready_to_seek_.set(ReadyToSeek::ProcessorThread::Converter, side);
        }

        sleep_for_ms(SLEEP_PERIOD_MS);
        continue;
      }

      AVFrameUniquePtr frame_filtered{av_frame_alloc(), avframe_deleter};

      if (filtered_frame_queues_[side]->pop(frame_filtered)) {
        if (display_ && display_->get_gpu_renderer_active()) {
          // GPU renderer: no CPU format conversion, but rescale sub-max frames
          // up to max_width_ × max_height_ so every uploaded texture has the
          // same dimensions. This is what keeps the libplacebo split-render's
          // src crop on an integer texel grid — mismatched-dim textures would
          // otherwise introduce fractional src_x1 values as the split moves,
          // producing visible 0–3 px cyclic stretch at the boundary.
          const bool dims_match = (static_cast<size_t>(frame_filtered->width) == format_converters_[side]->dest_width() &&
                                   static_cast<size_t>(frame_filtered->height) == format_converters_[side]->dest_height());
          if (dims_match) {
            const AVDictionaryEntry* gen = av_dict_get(frame_filtered->metadata, "filter_generation", nullptr, 0);
            const std::string frame_key = std::to_string(frame_filtered->pts) + ":" + (gen ? gen->value : "0");
            set_frame_key(frame_filtered.get(), frame_key);
            av_dict_set(&frame_filtered->metadata, "original_width", std::to_string(frame_filtered->width).c_str(), 0);
            av_dict_set(&frame_filtered->metadata, "original_height", std::to_string(frame_filtered->height).c_str(), 0);

            converted_frame_queues_[side]->push(std::move(frame_filtered));
          } else {
            AVFrameUniquePtr frame_converted{av_frame_alloc(), avframe_and_data_deleter};
            if (av_frame_copy_props(frame_converted.get(), frame_filtered.get()) < 0) {
              throw std::runtime_error("Copying filtered frame properties");
            }
            if (av_image_alloc(frame_converted->data, frame_converted->linesize, format_converters_[side]->dest_width(), format_converters_[side]->dest_height(), format_converters_[side]->dest_pixel_format(), 64) < 0) {
              throw std::runtime_error("Allocating rescaled picture");
            }
            (*format_converters_[side])(frame_filtered.get(), frame_converted.get());

            converted_frame_queues_[side]->push(std::move(frame_converted));
          }
        } else {
          // scale and convert pixel format before pushing to frame queue for displaying
          AVFrameUniquePtr frame_converted{av_frame_alloc(), avframe_and_data_deleter};

          if (av_frame_copy_props(frame_converted.get(), frame_filtered.get()) < 0) {
            throw std::runtime_error("Copying filtered frame properties");
          }
          if (av_image_alloc(frame_converted->data, frame_converted->linesize, format_converters_[side]->dest_width(), format_converters_[side]->dest_height(), format_converters_[side]->dest_pixel_format(), 64) < 0) {
            throw std::runtime_error("Allocating converted picture");
          }
          (*format_converters_[side])(frame_filtered.get(), frame_converted.get());

          converted_frame_queues_[side]->push(std::move(frame_converted));
        }
      } else if (filtered_frame_queues_[side]->is_stopped() || is_seeking(side)) {
        // Stop filtering
        converted_frame_queues_[side]->stop();
      }
    }
  } catch (...) {
    exception_holder_.store_current_exception();
    quit_all_queues();
  }
}

bool VideoCompare::keep_running() const {
  return !display_->get_quit() && !exception_holder_.has_exception();
}

void VideoCompare::quit_all_queues() {
  for (const auto& pair : demuxers_) {
    const Side& side = pair.first;

    converted_frame_queues_[side]->quit();
    filtered_frame_queues_[side]->quit();
    decoded_frame_queues_[side]->quit();
    packet_queues_[side]->quit();
  }
}

// Shared pipeline-barrier entry for main-thread-driven per-side operations
// (L1 re-decode, loop-mode materialize, auto-align candidate build).
//
// Raises the per-side seeking flag for each participating side, stops that
// side's packet queue, drains the downstream (decoded/filtered/converted)
// queues, and spin-waits until every processor thread for that side has parked
// at its ReadyToSeek flag. Once idle, clears the seeking flag so the main
// thread can drive the decoder without the decode worker's 10ms
// flush-while-seeking race. Queues remain stopped; the caller is responsible
// for the restore (decoder flush, filterer reinit, demuxer seek, queue
// restart) once its synchronous work is done.
void VideoCompare::enter_seek_barrier(const std::function<bool(const Side&)>& should_walk) {
  ready_to_seek_.reset_all();
  for (auto& pair : seeking_per_side_) {
    pair.second.store(should_walk(pair.first), std::memory_order_relaxed);
  }
  for (auto& pair : packet_queues_) {
    if (!should_walk(pair.first)) {
      continue;
    }
    pair.second->stop();
    pair.second->empty();
  }
  const auto drain_downstream = [&]() {
    for (auto& p : decoded_frame_queues_) {
      if (should_walk(p.first)) {
        p.second->empty();
      }
    }
    for (auto& p : filtered_frame_queues_) {
      if (should_walk(p.first)) {
        p.second->empty();
      }
    }
    for (auto& p : converted_frame_queues_) {
      if (should_walk(p.first)) {
        p.second->empty();
      }
    }
  };
  while (!ready_to_seek_.all_are_idle_where(should_walk)) {
    drain_downstream();
    sleep_for_ms(SLEEP_PERIOD_MS);
  }
  drain_downstream();
  // Clear seeking flags now that workers are parked. Leaving is_seeking true
  // while queues stay stopped would let the decode worker race main-thread
  // codec access via its 10ms flush loop.
  for (auto& pair : seeking_per_side_) {
    if (should_walk(pair.first)) {
      pair.second.store(false, std::memory_order_relaxed);
    }
  }
}

void VideoCompare::note_decoded_frame(const Side& side, const int64_t pts) {
  auto& detection_state = media_frame_detection_states_.at(side);
  auto& last_pts = detection_state.last_counted_pts;
  const int64_t previous_pts = last_pts.exchange(pts, std::memory_order_relaxed);

  if (previous_pts == pts) {
    return;
  }

  const int decoded_count = detection_state.decoded_count.fetch_add(1, std::memory_order_relaxed) + 1;

  if (decoded_count == 1) {
    detection_state.cardinality.store(MediaFrameCardinality::SingleFrame, std::memory_order_relaxed);
  } else if (decoded_count >= 2) {
    detection_state.cardinality.store(MediaFrameCardinality::MultiFrame, std::memory_order_relaxed);
  }
}

void VideoCompare::refresh_side_filter_metadata(const Side& side, const std::string& filters) {
  if (side.is_left()) {
    left_video_metadata_.set(MetadataProperties::FILTERS, filters);
  } else {
    right_video_info_[side].metadata.set(MetadataProperties::FILTERS, filters);
  }
}

bool VideoCompare::handle_pending_crop_request(const Side& active_right) {
  const PendingCropRequest crop_request = display_->get_and_clear_pending_crop_request();
  if (!crop_request.clear_requested && !crop_request.valid) {
    return false;
  }
  const Side target_right = crop_request.apply_right ? Side::Right(std::min(crop_request.right_target_index, right_video_info_.empty() ? 0UL : (right_video_info_.size() - 1))) : active_right;
  const bool swap_left_right = display_->get_swap_left_right();
  const Side resolved_left_side = swap_left_right ? active_right : LEFT;
  const Side resolved_right_side = swap_left_right ? LEFT : target_right;

  auto compose_crop_history = [&](const std::vector<SDL_Rect>& history) {
    SDL_Rect composed = {0, 0, 0, 0};
    bool initialized = false;
    for (const SDL_Rect& rect : history) {
      if (!initialized) {
        composed = rect;
        initialized = true;
        continue;
      }
      composed.x += rect.x;
      composed.y += rect.y;
      composed.w = rect.w;
      composed.h = rect.h;
    }
    return composed;
  };

  auto apply_crop_for_side = [&](const Side& side) {
    static constexpr int kMinCropDimension = 2;

    const int side_w = std::max(1, static_cast<int>(video_filterers_[side]->dest_width()));
    const int side_h = std::max(1, static_cast<int>(video_filterers_[side]->dest_height()));
    const int src_w = std::max(1, static_cast<int>(video_filterers_[side]->src_width()));
    const int src_h = std::max(1, static_cast<int>(video_filterers_[side]->src_height()));
    if (max_width_ == 0 || max_height_ == 0) {
      return false;
    }
    if (side_w < kMinCropDimension || side_h < kMinCropDimension || src_w < kMinCropDimension || src_h < kMinCropDimension) {
      return false;
    }
    const auto clamp_to = [](const int value, const int min_value, const int max_value) { return std::max(min_value, std::min(value, max_value)); };

    // When per_side_rects is set (auto-crop path), pick the rect for this
    // resolved side; otherwise fall back to the shared single-rect value.
    const SDL_Rect& source_rect = crop_request.per_side_rects
                                      ? (side == resolved_left_side ? crop_request.rect_left : crop_request.rect_right)
                                      : crop_request.rect;

    SDL_Rect mapped = {
        clamp_to(static_cast<int>(std::llround(static_cast<double>(source_rect.x) * side_w / max_width_)), 0, side_w - 1),
        clamp_to(static_cast<int>(std::llround(static_cast<double>(source_rect.y) * side_h / max_height_)), 0, side_h - 1),
        std::max(kMinCropDimension, static_cast<int>(std::llround(static_cast<double>(source_rect.w) * side_w / max_width_))),
        std::max(kMinCropDimension, static_cast<int>(std::llround(static_cast<double>(source_rect.h) * side_h / max_height_))),
    };
    mapped.w = std::min(mapped.w, side_w - mapped.x);
    mapped.h = std::min(mapped.h, side_h - mapped.y);
    if (mapped.w < kMinCropDimension || mapped.h < kMinCropDimension) {
      return false;
    }

    crop_history_[side].push_back(mapped);
    SDL_Rect composed = compose_crop_history(crop_history_[side]);
    composed.x = clamp_to(composed.x, 0, src_w - kMinCropDimension);
    composed.y = clamp_to(composed.y, 0, src_h - kMinCropDimension);
    composed.w = std::min(std::max(kMinCropDimension, composed.w), src_w - composed.x);
    composed.h = std::min(std::max(kMinCropDimension, composed.h), src_h - composed.y);
    if (composed.w < kMinCropDimension || composed.h < kMinCropDimension) {
      crop_history_[side].pop_back();
      return false;
    }

    const CropRect crop_rect{composed.x, composed.y, composed.w, composed.h};
    const bool changed = video_filterers_[side]->set_crop_rect(&crop_rect);

    return changed;
  };

  bool crop_changed = false;
  if (crop_request.clear_requested) {
    if (!crop_request.apply_left && !crop_request.apply_right) {
      for (auto& pair : video_filterers_) {
        crop_history_[pair.first].clear();
        crop_changed = pair.second->set_crop_rect(nullptr) || crop_changed;
      }
    } else {
      if (crop_request.apply_left) {
        crop_history_[resolved_left_side].clear();
        crop_changed = video_filterers_[resolved_left_side]->set_crop_rect(nullptr) || crop_changed;
      }
      if (crop_request.apply_right) {
        crop_history_[resolved_right_side].clear();
        crop_changed = video_filterers_[resolved_right_side]->set_crop_rect(nullptr) || crop_changed;
      }
    }
  } else if (crop_request.valid) {
    if (crop_request.apply_left) {
      crop_changed = apply_crop_for_side(resolved_left_side) || crop_changed;
    }
    if (crop_request.apply_right) {
      crop_changed = apply_crop_for_side(resolved_right_side) || crop_changed;
    }
  }

  if (!crop_changed) {
    return false;
  }

  scope_update_state_.reset();
  return true;
}

void VideoCompare::dump_debug_info(const int frame_number, const int64_t effective_right_time_shift, const int average_refresh_time) {
  std::cout << "FRAME: " << frame_number << std::endl;
  std::cout << "keep_running()=" << keep_running() << std::endl;
  std::cout << "has_exception()=" << exception_holder_.has_exception() << std::endl;
  std::cout << "seeking=" << any_seeking() << std::endl;
  std::cout << "effective_right_time_shift=" << effective_right_time_shift << std::endl;
  std::cout << "single_decoder_mode=" << single_decoder_mode_.enabled() << std::endl;
  std::cout << "average_refresh_time=" << average_refresh_time << std::endl;
  std::cout << "active_right_index=" << active_right_index_ << std::endl;

  const auto dump_queue = [](const std::string& side_name, const char* label, const auto& q) {
    std::cout << side_name << " " << label << ":"
              << " size=" << q->size()
              << ", is_stopped=" << q->is_stopped()
              << ", quit=" << q->is_quit()
              << std::endl;
  };
  for (const auto& pair : packet_queues_) {
    dump_queue(pair.first.to_string(), "packet demuxer", pair.second);
  }
  for (const auto& pair : packet_rings_) {
    const auto s = pair.second->stats();
    std::cout << pair.first.to_string() << " packet ring:"
              << " bytes=" << s.bytes_used << "/" << s.byte_budget
              << ", packets=" << s.packet_count
              << ", ranges=" << s.range_count
              << ", keyframes=" << s.keyframe_count
              << ", pts=[" << s.pts_min << "," << s.pts_max << "]"
              << (s.l1_disabled ? " [L1-disabled]" : "")
              << std::endl;
  }
  for (const auto& pair : decoded_frame_queues_) {
    dump_queue(pair.first.to_string(), "decoder", pair.second);
  }
  for (const auto& pair : filtered_frame_queues_) {
    dump_queue(pair.first.to_string(), "filterer", pair.second);
  }
  for (const auto& pair : converted_frame_queues_) {
    dump_queue(pair.first.to_string(), "format converter", pair.second);
  }
  for (const auto& pair : media_frame_detection_states_) {
    const MediaFrameCardinality cardinality = pair.second.cardinality.load(std::memory_order_relaxed);
    const char* cardinality_name =
        (cardinality == MediaFrameCardinality::Unknown) ? "Unknown"
        : (cardinality == MediaFrameCardinality::SingleFrame) ? "SingleFrame"
        : "MultiFrame";
    std::cout << pair.first.to_string() << " media frame cardinality: " << cardinality_name << std::endl;
  }

  std::cout << "all_are_idle()=" << ready_to_seek_.all_are_idle() << std::endl;

  std::cout << "--------------------------------------------------" << std::endl;
}

struct SideState {
  SideState(const Side& side, const Demuxer* demuxer, size_t ring_capacity) : side_(side), start_time_(demuxer->start_time() * AV_TIME_TO_SEC), ring(ring_capacity, ring_capacity), frame_duration_deque_(8) {
    if (start_time_ > 0) {
      sa_log_info(side, string_sprintf("Video has a start time of %s - timestamps will be shifted so they start at zero!", format_position(start_time_, true).c_str()));
    }
  }

  const Side side_;

  const float start_time_;

  // Symmetric history / current / prefetch display buffer. Populated from the pipeline
  // via intake_prefetch() every main-loop iteration.
  FrameRing ring;

  int64_t first_pts_ = INT64_MIN;
  int64_t pts_ = 0;
  int64_t delta_pts_ = 0;
  int32_t previous_decoded_picture_number_ = -1;
  int32_t decoded_picture_number_ = 0;
  int64_t effective_time_shift_ = 0;

  sorted_flat_deque<int64_t> frame_duration_deque_;

  int last_filter_generation_ = -1;
  std::string last_filter_description_;
};

void VideoCompare::compare() {
  try {
#ifdef _DEBUG
    std::string previous_state;
#endif

    // Create SideState for all videos. Each side's FrameRing uses frame_buffer_size_
    // for both history and prefetch capacities, so `+N` and `-N` have symmetric depth.
    std::map<Side, SideState> side_states;
    for (const auto& pair : demuxers_) {
      const Side& side = pair.first;
      const auto& demuxer = pair.second;

      side_states.emplace(std::piecewise_construct, std::forward_as_tuple(side), std::forward_as_tuple(side, demuxer.get(), frame_buffer_size_));
    }

    SideState& left = side_states.at(LEFT);
    // Use active right video
    Side active_right = Side::Right(active_right_index_);
    SideState* right_ptr = &side_states.at(active_right);

    int frame_offset = 0;

    int total_right_time_shifted = 0;

    int forward_navigate_frames = 0;

    bool auto_loop_triggered = false;

    const int max_digits = std::log10(frame_buffer_size_) + 1;
    const std::string frame_offset_format_str = string_sprintf("%%s%%0%dd/%%0%dd%%s", max_digits, max_digits);

    // for refreshing the display only
    Timer display_refresh_timer;
    sorted_flat_deque<uint32_t> refresh_time_deque(8);

    // for the full cycle
    Timer full_cycle_timer;
    sorted_flat_deque<uint32_t> full_cycle_time_deque(NOMINAL_FPS_UPDATE_RATE_US / 1000);

    std::string previous_frame_combo_tag;
    int32_t unique_frame_combo_tags_processed = 0;
    std::string fps_message = "Gathering stats... hold onto your pixels!";

    double next_refresh_at = 0;

    const bool log_event_routing = env_flag_enabled("VIDEO_COMPARE_LOG_EVENT_ROUTING");
    const bool show_packet_ring = env_flag_enabled("VIDEO_COMPARE_SHOW_PACKET_RING");
    const bool log_seek_timing = env_flag_enabled("VIDEO_COMPARE_LOG_SEEK_TIMING");
    const bool log_l1_stages = env_flag_enabled("VIDEO_COMPARE_LOG_L1_STAGES");

    // GOP heuristic: if the keyframe-to-target distance exceeds this many
    // seconds, fall back to L2. L1 decodes serially on the main thread; L2
    // uses the 4-stage pipeline in parallel plus a demuxer seek. For small
    // kf-to-target distances L1 wins; for large distances L2 wins. Default
    // 0.5 s — empirically around the crossover point on macOS HW decoders.
    //
    // Amortization: when we seed the ring's history with pre-target frames
    // (see L1 landing code below), subsequent backward presses hit L0 pivots,
    // so one L1 fire covers up to frame_buffer_size_ presses. The threshold
    // accounts for that by being tolerant of single-press cases at the cost
    // of more-aggressive mashing scenarios (which amortize regardless).
    double l1_max_kf_distance_sec = 0.5;
    if (const char* raw = std::getenv("VIDEO_COMPARE_L1_MAX_KF_DISTANCE_SEC"); raw && *raw) {
      try { l1_max_kf_distance_sec = std::stod(raw); } catch (...) {}
    }

    // Loop-mode eager-materialize cap: how many seconds of PacketRing content
    // to decode into the FrameRing when the user enters `,` / `.` loop mode.
    // Default 5 s. At 4K HDR (≈47.5 MiB/frame × 60 fps) that's ≈14 GiB — set
    // lower (or use lower-res content) if memory is tight. At 1080p RGB24 5 s
    // at 30 fps is ≈940 MiB per side.
    double loop_cap_sec = 5.0;
    if (const char* raw = std::getenv("VIDEO_COMPARE_LOOP_CAP_SEC"); raw && *raw) {
      try { loop_cap_sec = std::stod(raw); } catch (...) {}
    }
    // Track loop-mode transitions so we can materialize on entry and shrink
    // the ring back down on exit.
    Display::Loop previous_loop_mode = Display::Loop::Off;

    for (uint64_t frame_number = 0;; ++frame_number) {
      // Set FPS message if needed. GPU renderer shows persistent FPS
      // counters instead, so skip the toast there (it would re-trigger every
      // iteration and never fade).
      if (display_->get_show_fps() && !display_->get_gpu_renderer_active()) {
        display_->set_pending_message(fps_message);
      }

      full_cycle_timer.update();

      // Event model:
      // - Only *one* place pumps SDL events (this main loop).
      // - Scope windows may consume events.
      // - Destruction is deferred: scope windows set close_requested_ and are destroyed later by reconcile().
      display_->begin_input_frame();
      SDL_Event event;
      while (SDL_PollEvent(&event) != 0) {
        display_->mark_input_received();

        const uint32_t wid = SDLEventInfo::window_id(event);
        const bool consumed_by_scope = scope_manager_->handle_event(event);
        if (!consumed_by_scope) {
          display_->handle_event(event);
        }

        if (log_event_routing) {
          std::cerr << "[event]"
                    << " type=" << SDLEventInfo::type_name(event.type) << " (" << event.type << ")"
                    << " windowID=" << wid
                    << " -> " << (consumed_by_scope ? "scope" : "display")
                    << std::endl;
        }
      }

      // Handle scope windows
      const SDL_Rect roi = display_->get_visible_roi_in_single_frame_coordinates();
      const ScopeWindow::Roi scope_window_roi{roi.x, roi.y, roi.w, roi.h};

      for (const auto type : ScopeWindow::all_types()) {
        if (display_->get_toggle_scope_window_requested(type)) {
          const bool opened = scope_manager_->request_toggle(type);
          if (opened) {
            // Ensure the main window retains keyboard focus after opening a scope
            display_->focus_main_window();
            scope_update_state_.reset();
          }
        }
      }

      scope_manager_->set_roi(scope_window_roi);
      scope_manager_->reconcile();
      if (scope_manager_->has_fatal_error()) {
        throw std::runtime_error(scope_manager_->fatal_error_message());
      }
      if (scope_manager_->consume_refresh_request()) {
        scope_update_state_.reset();
      }

      if (!keep_running()) {
        break;
      }

#ifdef _DEBUG
      if ((frame_number % 100) == 0) {
        dump_debug_info(frame_number, right_ptr->effective_time_shift_, refresh_time_deque.average());
      }
#endif

      const int format_conversion_sws_flags = determine_sws_flags(display_->get_fast_input_alignment());
      // Update active right video index from display and switch if changed
      size_t new_active_index = display_->get_active_right_index();
      if (new_active_index != active_right_index_) {
        active_right_index_ = new_active_index;
        active_right = Side::Right(active_right_index_);
        right_ptr = &side_states.at(active_right);

        display_->update_right_video(right_video_info_[active_right].file_name, right_video_info_[active_right].metadata);
        scope_update_state_.reset();
      }
      // Update format converter flags for all videos
      for (auto& pair : format_converters_) {
        pair.second->set_pending_flags(format_conversion_sws_flags);
      }

      // Allow 50 ms of lag without resetting timer (and ticking playback)
      if (display_->get_tick_playback() || (display_->get_possibly_tick_playback() && (timer_->us_until_target() < -50000))) {
        timer_->reset();
      }

      const int frame_navigation_delta = display_->get_frame_navigation_delta();

      // Normalize delta values to a sane fallback so we can reuse them for seeks/time shifts.
      const auto normalized_delta = [](const int64_t delta) { return delta > 0 ? delta : 10000; };
      const int64_t right_delta = normalized_delta(right_ptr->delta_pts_);
      const int64_t left_or_right_delta = (left.delta_pts_ > 0) ? left.delta_pts_ : right_delta;

      // Positive delta means "decode N next frames" (shift+D).
      if (frame_navigation_delta > 0) {
        forward_navigate_frames += frame_navigation_delta;
      }

      float seek_relative = display_->get_seek_relative();
      bool seek_from_start = display_->get_seek_from_start();

      // Negative delta means "seek backward by N frames" (shift+A) using average frame duration.
      if (frame_navigation_delta < 0) {
        seek_relative += static_cast<float>(frame_navigation_delta) * (static_cast<float>(left_or_right_delta) * AV_TIME_TO_SEC);
        seek_from_start = false;
      }

      bool skip_update = false;

      // Drain the pipeline's converted frame output into each ring's prefetch tail.
      // Non-blocking: whatever the converter has produced so far lands in the ring,
      // and the rest is collected next iteration. Running this BEFORE the seek-block
      // dispatch lets a forward `+N` pivot see prefetched frames that arrived since
      // the previous iteration, so the pivot ceiling matches prefetch_capacity rather
      // than the raw converter queue depth.
      auto intake_prefetch = [&]() {
        for (auto& pair : side_states) {
          SideState& ss = pair.second;
          const AVFrame* const current_frame = ss.ring.current_frame();
          const int64_t min_acceptable_pts = (current_frame != nullptr) ? current_frame->pts : INT64_MIN;
          while (!ss.ring.prefetch_full()) {
            AVFrameUniquePtr frame{nullptr, avframe_deleter};
            if (!converted_frame_queues_[ss.side_]->try_pop(frame) || frame == nullptr) {
              break;
            }
            // Drop frames whose pts is at or before the ring's current frame.
            // This filter lets L1 / auto-align post-seek use a backward demuxer
            // seek (decoder-safe GOP boundary) without the worker's re-emitted
            // pre-target frames corrupting the ring prefetch's forward ordering
            // — a subsequent `+`/pivot_forward would otherwise land on a frame
            // visually behind the current cursor.
            if (frame->pts <= min_acceptable_pts) {
              continue;
            }
            if (!ss.ring.push_prefetch(std::move(frame))) {
              break;
            }
          }
        }
      };
      intake_prefetch();

      // Trim each side's PacketRing to its byte budget. Safe to call every tick:
      // no-op when under budget; the demuxer thread producer is mutex-synced.
      // The packet ring stores raw demuxer-PTS; we protect the current displayed
      // frame's PTS (same time base as packet PTS). Before the first frame
      // arrives we pass INT64_MIN, which keeps everything while still enforcing
      // byte budget by dropping farthest ranges.
      for (auto& pair : side_states) {
        SideState& ss = pair.second;
        auto it = packet_rings_.find(ss.side_);
        if (it == packet_rings_.end() || !it->second) continue;
        const AVFrame* current = ss.ring.current_frame();
        const int64_t protect_pts = (current != nullptr) ? current->pts : INT64_MIN;
        it->second->evict_to_budget(protect_pts);
      }

      // Periodic PacketRing readout (env-gated; off by default).
      if (show_packet_ring && (frame_number % 60) == 0) {
        for (const auto& pair : packet_rings_) {
          const auto s = pair.second->stats();
          std::cerr << "[packet-ring " << pair.first.to_string() << "]"
                    << " bytes=" << s.bytes_used << "/" << s.byte_budget
                    << " packets=" << s.packet_count
                    << " ranges=" << s.range_count
                    << " keyframes=" << s.keyframe_count
                    << " pts=[" << s.pts_min << "," << s.pts_max << "]"
                    << (s.l1_disabled ? " L1-DISABLED" : "")
                    << std::endl;
        }
      }

      // Loop-mode eager-materialize on entry, shrink on exit.
      //
      // When the user presses `,` or `.` (or when auto-loop fires), the decoded
      // FrameRing only holds ≈frame_buffer_size_ frames — way too short to be
      // useful for visual comparison. On mode entry we barrier the pipeline,
      // grow each side's FrameRing, and eagerly decode the most recent
      // `loop_cap_sec` seconds from the PacketRing into the ring's history.
      // On exit we shrink the ring back to `frame_buffer_size_`.
      {
        const Display::Loop current_loop_mode = display_->get_buffer_play_loop_mode();
        if (log_seek_timing && current_loop_mode != previous_loop_mode) {
          std::cerr << "[loop] mode transition " << static_cast<int>(previous_loop_mode) << "→" << static_cast<int>(current_loop_mode) << std::endl;
        }
        if (previous_loop_mode == Display::Loop::Off && current_loop_mode != Display::Loop::Off) {
          // === Entering loop mode: materialize ===
          // Capture per-side current frame PTS (AV_TIME_BASE μs since start) so
          // we know where to stop decoding.
          std::map<Side, int64_t> current_frame_pts;
          for (auto& p : side_states) {
            const AVFrame* cf = p.second.ring.current_frame();
            if (cf != nullptr) current_frame_pts[p.first] = cf->pts;
          }
          if (!current_frame_pts.empty()) {
            if (log_seek_timing) std::cerr << "[loop] materialize begin, cap=" << loop_cap_sec << "s" << std::endl;
            display_->set_pending_message("Loop: decoding buffered range…");
            const auto loop_t_start = std::chrono::steady_clock::now();

            // --- Barrier (shared with L1 and auto-align) ---
            enter_seek_barrier([](const Side&) { return true; });

            // --- Reinit filterers (close_src was called during barrier) ---
            for (auto& p : video_filterers_) {
              p.second->consume_filter_change();
              p.second->reinit();
            }

            // --- Per-side eager decode ---
            // Capacity: enough for cap_sec seconds at a generous frame-rate
            // assumption. Excess capacity is harmless — FrameRing only holds
            // what gets pushed.
            const double framerate_guess = 60.0;
            const size_t frame_count_guess = static_cast<size_t>(std::ceil(loop_cap_sec * framerate_guess)) + 8;
            const int64_t cap_us = static_cast<int64_t>(loop_cap_sec * AV_TIME_BASE);

            try {
              for (auto& p : side_states) {
                const Side& side = p.first;
                SideState& ss = p.second;
                auto cfp_it = current_frame_pts.find(side);
                if (cfp_it == current_frame_pts.end()) continue;
                const int64_t target_us = cfp_it->second;
                const int64_t start_us = target_us - cap_us;

                auto ring_it = packet_rings_.find(side);
                if (ring_it == packet_rings_.end() || !ring_it->second) continue;

                const AVRational stream_tb = demuxers_[side]->time_base();
                const int64_t demuxer_start_us = demuxers_[side]->start_time();
                // Convert loop-start (μs since start) back to stream tb for
                // PacketRing lookup: add start_time to get packet-relative, then
                // rescale. Clamp to 0 so we never ask for negative stream pts.
                const int64_t start_raw = av_rescale_q(std::max<int64_t>(start_us + demuxer_start_us, 0), AV_TIME_BASE_Q, stream_tb);
                auto hit = ring_it->second->keyframe_at_or_before(start_raw);
                if (!hit.has_value()) continue;  // no keyframe; skip side

                VideoDecoder& dec = *video_decoders_[side];
                VideoFilterer& flt = *video_filterers_[side];
                FormatConverter& cvt = *format_converters_[side];
                dec.flush();
                dec.reset_pts_state();

                ss.ring.clear();
                ss.ring.set_capacities(frame_count_guess, frame_count_guess);

                const bool gpu_on = (display_ && display_->get_gpu_renderer_active());
                // Push one decoded frame into the ring's prefetch if within loop
                // range. Returns true when we've passed target_us (past end of
                // loop); caller should stop feeding packets.
                auto push_decoded = [&](AVFrame* decoded_raw) -> bool {
                  AVFrameSharedPtr decoded_sw;
                  if (decoded_raw->format == dec.hw_pixel_format()) {
                    decoded_sw = AVFrameSharedPtr{av_frame_alloc(), avframe_deleter};
                    if (av_hwframe_transfer_data(decoded_sw.get(), decoded_raw, 0) < 0) return false;
                    if (av_frame_copy_props(decoded_sw.get(), decoded_raw) < 0) return false;
                  } else {
                    decoded_sw = AVFrameSharedPtr{av_frame_clone(decoded_raw), avframe_deleter};
                    if (!decoded_sw) return false;
                  }
                  if (!flt.send(decoded_sw.get())) return false;
                  while (true) {
                    AVFrameUniquePtr filtered{av_frame_alloc(), avframe_deleter};
                    if (!flt.receive(filtered.get())) break;
                    AVFrameUniquePtr out;
                    if (gpu_on) {
                      const bool dims_match = (static_cast<size_t>(filtered->width) == cvt.dest_width() && static_cast<size_t>(filtered->height) == cvt.dest_height());
                      if (dims_match) {
                        const AVDictionaryEntry* gen = av_dict_get(filtered->metadata, "filter_generation", nullptr, 0);
                        const std::string frame_key = std::to_string(filtered->pts) + ":" + (gen ? gen->value : "0");
                        set_frame_key(filtered.get(), frame_key);
                        av_dict_set(&filtered->metadata, "original_width", std::to_string(filtered->width).c_str(), 0);
                        av_dict_set(&filtered->metadata, "original_height", std::to_string(filtered->height).c_str(), 0);
                        out = AVFrameUniquePtr{filtered.release(), avframe_deleter};
                      } else {
                        AVFrameUniquePtr rescaled{av_frame_alloc(), avframe_and_data_deleter};
                        if (av_frame_copy_props(rescaled.get(), filtered.get()) < 0) return false;
                        if (av_image_alloc(rescaled->data, rescaled->linesize, cvt.dest_width(), cvt.dest_height(), cvt.dest_pixel_format(), 64) < 0) return false;
                        cvt(filtered.get(), rescaled.get());
                        out = std::move(rescaled);
                      }
                    } else {
                      AVFrameUniquePtr converted{av_frame_alloc(), avframe_and_data_deleter};
                      if (av_frame_copy_props(converted.get(), filtered.get()) < 0) return false;
                      if (av_image_alloc(converted->data, converted->linesize, cvt.dest_width(), cvt.dest_height(), cvt.dest_pixel_format(), 64) < 0) return false;
                      cvt(filtered.get(), converted.get());
                      out = std::move(converted);
                    }
                    if (out->pts > target_us) return true;  // past end
                    if (out->pts < start_us) continue;       // before start
                    // In range: push to prefetch tail.
                    if (!ss.ring.push_prefetch(std::move(out))) break;
                  }
                  return false;
                };

                bool stop = false;
                hit->range->iterate_from(hit->absolute_buffer_index, [&](const AVPacket* src_pkt) -> bool {
                  if (stop) return false;
                  AVPacket* cloned = av_packet_clone(src_pkt);
                  if (!cloned) return false;
                  dec.send(cloned);
                  av_packet_free(&cloned);
                  while (!stop) {
                    AVFrame* raw = av_frame_alloc();
                    if (!raw) return false;
                    if (!dec.receive(raw, demuxers_[side].get())) {
                      av_frame_free(&raw);
                      break;
                    }
                    const bool past = push_decoded(raw);
                    av_frame_free(&raw);
                    if (past) stop = true;
                  }
                  return !stop;
                });
                if (!stop) {
                  dec.send(nullptr);
                  while (!stop) {
                    AVFrame* raw = av_frame_alloc();
                    if (!raw) break;
                    if (!dec.receive(raw, demuxers_[side].get())) {
                      av_frame_free(&raw);
                      break;
                    }
                    const bool past = push_decoded(raw);
                    av_frame_free(&raw);
                    if (past) stop = true;
                  }
                }

                // Shift all prefetched frames through to fill history with the
                // oldest-first layout. After the loop, current = newest decoded
                // frame and history contains the rest in reverse-time order.
                while (ss.ring.prefetch_size() > 0) ss.ring.advance();
              }
            } catch (const std::exception& ex) {
              std::cerr << "[loop-materialize] exception: " << ex.what() << std::endl;
            } catch (...) {
              std::cerr << "[loop-materialize] unknown exception" << std::endl;
            }

            // Fix up per-side SideState bookkeeping. Use each side's final
            // current frame (post-materialize) to re-establish pts_.
            for (auto& p : side_states) {
              const Side& side = p.first;
              SideState& ss = p.second;
              AVFrame* cur_frame = ss.ring.current_frame();
              if (cur_frame == nullptr) continue;
              if (side.is_left()) {
                ss.effective_time_shift_ = 0;
                ss.pts_ = cur_frame->pts;
              } else {
                ss.effective_time_shift_ = time_shifter_.static_shift() + time_shifter_.dynamic_shift(cur_frame->pts);
                ss.pts_ = cur_frame->pts - ss.effective_time_shift_;
              }
              ss.previous_decoded_picture_number_ = -1;
              ss.decoded_picture_number_ = 1;
            }

            // Post-materialize cleanup: flush decoders + reinit filterers so the
            // pipeline resumes clean; seek demuxers just past current so
            // packet_queues_ refill from there on loop exit.
            for (auto& p : video_decoders_) {
              p.second->flush();
              p.second->reset_pts_state();
            }
            for (auto& p : video_filterers_) p.second->reinit();
            for (auto& p : demuxers_) {
              const Side& side = p.first;
              auto cfp_it = current_frame_pts.find(side);
              if (cfp_it == current_frame_pts.end()) continue;
              const double start_time_sec = static_cast<double>(demuxers_[side]->start_time()) * AV_TIME_TO_SEC;
              const double target_sec = (static_cast<double>(cfp_it->second) * AV_TIME_TO_SEC) + start_time_sec + 0.001;
              p.second->seek(static_cast<float>(target_sec), true);
            }
            for (auto& p : packet_queues_) p.second->restart();
            for (auto& p : decoded_frame_queues_) p.second->restart();
            for (auto& p : filtered_frame_queues_) p.second->restart();
            for (auto& p : converted_frame_queues_) p.second->restart();

            const auto loop_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - loop_t_start).count();
            const int left_span = left.ring.history_plus_current_size();
            const int right_span = right_ptr->ring.history_plus_current_size();
            display_->set_pending_message(string_sprintf("Loop: %.1fs buffered (%d/%d frames, %lldms)", loop_cap_sec, left_span, right_span, static_cast<long long>(loop_elapsed)));
            if (log_seek_timing) {
              std::cerr << "[loop] materialize done in " << loop_elapsed << "ms"
                        << ", left=" << left_span << " right=" << right_span << " frames"
                        << std::endl;
            }
            frame_offset = 0;
          }
        } else if (previous_loop_mode != Display::Loop::Off && current_loop_mode == Display::Loop::Off) {
          // === Exiting loop mode: shrink FrameRing back to frame_buffer_size_ ===
          // set_capacities evicts extras from the back of each deque, keeping
          // the most recent frames around the cursor.
          for (auto& p : side_states) {
            p.second.ring.set_capacities(frame_buffer_size_, frame_buffer_size_);
          }
          display_->set_pending_message("Loop: exit");
        }
        previous_loop_mode = current_loop_mode;
      }

      // handle HDR display state change (window moved between HDR/SDR displays)
      const bool hdr_changed = handle_hdr_state_change();

      // handle pending crop request
      const bool force_seek_current_position = handle_pending_crop_request(active_right) || hdr_changed;

      int shift_right_frames = display_->get_shift_right_frames();

      // Auto-align: find the right-side frame whose content best matches left's
      // current position, using a windowed multi-probe structural match to reject
      // motion-aliasing false positives. Folds the resulting frame delta into
      // shift_right_frames so the standard L0/L1/L2 seek dispatch handles the
      // actual motion. Post-seek verification (populated into
      // pending_auto_align_verification_) reruns the score once the seek lands.
      //
      // Window extents vary by mode:
      //   Symmetric (` key): ±kAutoAlignSymmetricHalfSec around left.current.
      //   Backward  ([ key): [-kAutoAlignDirectionalSec, 0] relative to left.current.
      //   Forward   (] key): [0, +kAutoAlignDirectionalSec] relative to left.current.
      // If the right ring already spans the window (typical for Symmetric), the
      // search runs entirely on ring-resident frames with no decode cost. When
      // the window exceeds the ring (typical for Backward/Forward), a barriered
      // PacketRing walk decodes the uncovered portion inline and discards the
      // frames after fingerprinting — the FrameRing is untouched.
      if (display_->get_auto_align_requested()) {
        // Tunables. Kept as locals (not CLI flags) until field data says otherwise.
        constexpr float kAutoAlignSymmetricBaseWidthSec = 0.5f;    // per-press expansion for `
        constexpr float kAutoAlignDirectionalBaseWidthSec = 1.0f;  // per-press expansion for [ or ]
        constexpr float kAutoAlignConfidenceFloor = 0.60f;         // below this, don't seek
        constexpr float kAutoAlignImprovementEps = 0.005f;         // `: "strictly stronger" means > current + eps
        // Tie-break band: two scores within this are treated as equal for the
        // "prefer nearest-to-current" tie-break. Much tighter than the
        // improvement eps because a sharp 1.000 peak can be beaten by a
        // near-peer only a few frames closer to current — we want the peak
        // to win whenever it's even slightly stronger.
        constexpr float kAutoAlignTieBreakBand = 0.0005f;
        // Candidates within this of 1.0 short-circuit directional selection:
        // once a near-perfect match exists in the pressed direction, it wins
        // regardless of its distance from current.
        constexpr float kAutoAlignShortCircuitEps = 0.002f;
        constexpr int kAutoAlignProbeRadius = 2;                   // probes at k ∈ {-2..+2}
        const bool log_auto_align = env_flag_enabled("VIDEO_COMPARE_LOG_AUTO_ALIGN");

        // Resolve the mode into a base width + directionality (which ends of
        // the searched interval this press is allowed to grow). The actual
        // window on the follower axis is derived below from the retry cache's
        // searched interval plus one mode_base_width of growth.
        const AutoAlignMode auto_align_mode = display_->get_auto_align_mode();
        const char* mode_label = "sym";
        float mode_base_width_sec = kAutoAlignSymmetricBaseWidthSec;
        bool mode_grows_low = true;
        bool mode_grows_high = true;
        if (auto_align_mode == AutoAlignMode::Backward) {
          mode_label = "back";
          mode_base_width_sec = kAutoAlignDirectionalBaseWidthSec;
          mode_grows_low = true;
          mode_grows_high = false;
        } else if (auto_align_mode == AutoAlignMode::Forward) {
          mode_label = "fwd";
          mode_base_width_sec = kAutoAlignDirectionalBaseWidthSec;
          mode_grows_low = false;
          mode_grows_high = true;
        }
        const int64_t mode_base_width_pts = static_cast<int64_t>(static_cast<double>(mode_base_width_sec) * AV_TIME_BASE);

        // Follower is the side the user means when they press a "right video"
        // input; under swap that's underlying LEFT, otherwise underlying RIGHT.
        // Master is the opposite side — the stationary reference whose frames
        // supply probes. Every ring/decoder/delta-pts reference below goes
        // through these aliases so the algorithm is side-agnostic.
        const Side auto_align_follower_side = display_->follower_side_for_input();
        SideState& master_state = auto_align_follower_side.is_right() ? left : *right_ptr;
        SideState& follower_state = auto_align_follower_side.is_right() ? *right_ptr : left;
        const Side master_side = master_state.side_;

        const AVFrame* const master_current = master_state.ring.current_frame();
        const AVFrame* const follower_current = follower_state.ring.current_frame();
        const int64_t master_delta_pts = master_state.delta_pts_;
        const int64_t follower_delta_pts = follower_state.delta_pts_;

        if (master_current == nullptr || follower_current == nullptr || master_delta_pts <= 0 || follower_delta_pts <= 0) {
          display_->set_pending_message("Auto-align: no frames available");
          if (log_auto_align) {
            std::cerr << "[auto-align] decision=no_frames" << std::endl;
          }
        } else {
          // Look up (or build) the 64x64 GRAY8 SwsContext for the given source
          // format and dimensions. Contexts are cached for the life of
          // VideoCompare, keyed by (format, width, height).
          const auto get_or_build_sws = [this](AVPixelFormat fmt, int w, int h) -> SwsContext* {
            const AutoAlignSwsKey key{fmt, w, h};
            const auto it = auto_align_sws_cache_.find(key);
            if (it != auto_align_sws_cache_.end()) {
              return it->second.get();
            }
            SwsContext* raw = sws_getContext(w, h, fmt, MetricsCalculator::kFingerprintSize, MetricsCalculator::kFingerprintSize, AV_PIX_FMT_GRAY8, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (raw == nullptr) {
              return nullptr;
            }
            const auto ins = auto_align_sws_cache_.emplace(key, SwsContextUniquePtr(raw));
            return ins.first->second.get();
          };

          // Fingerprint a ring-resident frame (post-filter + post-converter;
          // packed RGB24/RGB48LE/similar). The swscale call reads data[0]/
          // linesize[0] and ignores the chroma plane slots.
          const auto fingerprint_ring_frame = [&](const AVFrame* frame, std::vector<float>& out) -> bool {
            if (frame == nullptr || frame->width <= 0 || frame->height <= 0) {
              return false;
            }
            SwsContext* const ctx = get_or_build_sws(static_cast<AVPixelFormat>(frame->format), frame->width, frame->height);
            if (ctx == nullptr) {
              return false;
            }
            MetricsCalculator::compute_structural_fingerprint(frame->data, frame->linesize, frame->height, ctx, out);
            return !out.empty();
          };

          // Find the frame in `ring` whose PTS is closest to target_pts. Returns
          // nullptr if no frame is within `tolerance_pts`.
          const auto find_nearest_in_ring = [](const FrameRing& ring, int64_t target_pts, int64_t tolerance_pts) -> const AVFrame* {
            const int min_off = -ring.history_size();
            const int max_off = ring.prefetch_size();
            const AVFrame* best = nullptr;
            int64_t best_dist = std::numeric_limits<int64_t>::max();
            for (int off = min_off; off <= max_off; ++off) {
              const AVFrame* const f = ring.at(off);
              if (f == nullptr) {
                continue;
              }
              const int64_t dist = std::abs(f->pts - target_pts);
              if (dist < best_dist) {
                best_dist = dist;
                best = f;
              }
            }
            if (best == nullptr || best_dist > tolerance_pts) {
              return nullptr;
            }
            return best;
          };

          using Candidate = AutoAlignCandidate;

          // Decide whether the previous press's cached candidates/probes are
          // still valid for this press. The cache is invalidated when:
          //   - master or follower position moved outside what auto-align
          //     produced (manual seek, scrub, +/- frame shift);
          //   - user swapped follower sides.
          // Mode-change does NOT invalidate: pressing ` then [ keeps the
          // candidates and master probes from the first press, and just
          // expands the searched interval in the new mode's allowed
          // direction(s). Post-seek preservation is handled at cache-update
          // time below (master_pts stays, follower_pts advances to landed).
          AutoAlignRetryCache& retry = auto_align_retry_cache_;
          const bool retry_context_matches = retry.valid &&
                                             retry.follower_side == auto_align_follower_side &&
                                             retry.master_pts_at_cache == master_current->pts &&
                                             retry.follower_pts_at_cache == follower_current->pts;
          const bool can_reuse_cache = retry_context_matches;

          // Query follower clip bounds for boundary-aware expansion. PTS on
          // the follower axis is start-time-normalized (see demuxer_start_us
          // subtraction in the walk below), so the valid range is [0, duration).
          int64_t follower_clip_min_pts = 0;
          int64_t follower_clip_max_pts = std::numeric_limits<int64_t>::max();
          {
            auto demuxer_it = demuxers_.find(follower_state.side_);
            if (demuxer_it != demuxers_.end() && demuxer_it->second) {
              const int64_t duration_us = demuxer_it->second->duration();
              if (duration_us > 0) {
                follower_clip_max_pts = duration_us;
              }
            }
          }

          // --- Compute searched interval for this press ---
          // The cache stores [searched_low_pts, searched_high_pts] on the
          // follower's PTS axis. Each press grows the interval in its mode's
          // allowed direction(s) by one mode_base_width, clamped to the clip
          // bounds. Saturation flags record which directions have reached a
          // boundary and can't grow further.
          int64_t searched_low_pts;
          int64_t searched_high_pts;
          bool low_saturated;
          bool high_saturated;
          bool new_work_low = false;
          bool new_work_high = false;
          const int64_t follower_current_pts = follower_current->pts;
          if (can_reuse_cache) {
            searched_low_pts = retry.searched_low_pts;
            searched_high_pts = retry.searched_high_pts;
            low_saturated = retry.low_saturated;
            high_saturated = retry.high_saturated;
            if (mode_grows_low && !low_saturated) {
              int64_t new_low = searched_low_pts - mode_base_width_pts;
              if (new_low <= follower_clip_min_pts) {
                new_low = follower_clip_min_pts;
                low_saturated = true;
              }
              if (new_low < searched_low_pts) {
                searched_low_pts = new_low;
                new_work_low = true;
              }
            }
            if (mode_grows_high && !high_saturated) {
              int64_t new_high = searched_high_pts + mode_base_width_pts;
              if (new_high >= follower_clip_max_pts) {
                new_high = follower_clip_max_pts;
                high_saturated = true;
              }
              if (new_high > searched_high_pts) {
                searched_high_pts = new_high;
                new_work_high = true;
              }
            }
          } else {
            // Fresh cache: seed from follower_current extended by mode's base
            // width on each allowed side, clamped to clip bounds.
            searched_low_pts = follower_current_pts;
            searched_high_pts = follower_current_pts;
            if (mode_grows_low) {
              const int64_t target_low = follower_current_pts - mode_base_width_pts;
              searched_low_pts = std::max(follower_clip_min_pts, target_low);
              // new_work reflects actual expansion, not just mode intent —
              // when follower_current is at the clip start, [ couldn't
              // expand at all and should report saturation instead.
              new_work_low = (searched_low_pts < follower_current_pts);
            }
            if (mode_grows_high) {
              const int64_t target_high = follower_current_pts + mode_base_width_pts;
              searched_high_pts = std::min(follower_clip_max_pts, target_high);
              new_work_high = (searched_high_pts > follower_current_pts);
            }
            low_saturated = mode_grows_low && (searched_low_pts <= follower_clip_min_pts);
            high_saturated = mode_grows_high && (searched_high_pts >= follower_clip_max_pts);
          }

          {
            // --- Build the follower-side candidate pool ---
          // Seed from cache (if reusing), then top up from the current ring.
          // The ring contribution is deduped against whatever cache provided,
          // so ring frames already fingerprinted last press are skipped.
          std::vector<Candidate> candidates;
          std::unordered_set<int64_t> already_fingerprinted_pts;
          int ring_contributed = 0;
          if (can_reuse_cache) {
            candidates = std::move(retry.candidates);
            for (const auto& c : candidates) {
              already_fingerprinted_pts.insert(c.pts);
            }
          }
          {
            const FrameRing& fring = follower_state.ring;
            const int min_off = -fring.history_size();
            const int max_off = fring.prefetch_size();
            candidates.reserve(candidates.size() + static_cast<size_t>(max_off - min_off + 1));
            for (int off = min_off; off <= max_off; ++off) {
              const AVFrame* const f = fring.at(off);
              if (f == nullptr) {
                continue;
              }
              if (already_fingerprinted_pts.count(f->pts) > 0) {
                continue;
              }
              Candidate c;
              c.pts = f->pts;
              if (!fingerprint_ring_frame(f, c.fp)) {
                continue;
              }
              already_fingerprinted_pts.insert(c.pts);
              candidates.push_back(std::move(c));
              ++ring_contributed;
            }
          }

          // Master-axis PTS used throughout scoring (for delta-t hypotheses).
          const int64_t master_current_pts = master_current->pts;

          // The scoring window is the searched interval on the follower axis
          // (absolute PTS) extended outward with ring + probe slack so that
          // probes near the interval's edges can find their partner frames.
          // Slack doesn't contribute to the user-visible searched_window: it's
          // invisible padding that makes the edge candidates scorable.
          const int64_t approx_probe_step_pts = std::max(master_delta_pts, follower_delta_pts);
          const int64_t probe_edge_slack_pts = static_cast<int64_t>(kAutoAlignProbeRadius + 1) * approx_probe_step_pts;
          // Capacity-based slack (not current count): after a fresh L2 seek the
          // ring's prefetch is empty, so prefetch_size() would collapse the
          // extension and truncate the walk. Capacity guarantees the walk
          // covers what the ring will hold once intake_prefetch refills.
          const int64_t ring_forward_slack_pts = static_cast<int64_t>(follower_state.ring.prefetch_capacity()) * follower_delta_pts;
          const int64_t ring_backward_slack_pts = static_cast<int64_t>(follower_state.ring.history_capacity()) * follower_delta_pts;
          int64_t window_start_pts = searched_low_pts - ring_backward_slack_pts - probe_edge_slack_pts;
          int64_t window_end_pts = searched_high_pts + ring_forward_slack_pts + probe_edge_slack_pts;
          // Also ensure the follower's current position is always in-window
          // even if the searched interval was seeded/expanded only on one side.
          window_start_pts = std::min(window_start_pts, follower_current_pts - probe_edge_slack_pts);
          window_end_pts = std::max(window_end_pts, follower_current_pts + probe_edge_slack_pts);

          // Figure out how much of the requested window the candidate pool
          // already covers. If at least one candidate is at-or-before
          // window_start_pts AND one is at-or-after window_end_pts, the pool
          // spans the window — skip the barriered PacketRing walk entirely.
          // Pool here includes both the current ring AND any cache carried
          // forward from a prior low-confidence press.
          int64_t pool_min_pts = std::numeric_limits<int64_t>::max();
          int64_t pool_max_pts = std::numeric_limits<int64_t>::min();
          for (const auto& c : candidates) {
            pool_min_pts = std::min(pool_min_pts, c.pts);
            pool_max_pts = std::max(pool_max_pts, c.pts);
          }
          const bool pool_covers_window = !candidates.empty() && pool_min_pts <= window_start_pts && pool_max_pts >= window_end_pts;

          // --- PacketRing walk (only when ring coverage is insufficient) ---
          // Mirrors the L1 re-decode pattern: barrier the follower pipeline,
          // flush the decoder, walk PacketRing packets from the keyframe ≤
          // window_start forward, and fingerprint each decoded frame whose PTS
          // lands inside the window. Frames already present in the ring are
          // skipped (same PTS → already fingerprinted).
          int decoded_added = 0;
          int decode_ms = 0;
          bool walk_attempted = false;
          const char* walk_skip_reason = nullptr;
          if (!pool_covers_window) {
            walk_attempted = true;
            const Side follower_side = follower_state.side_;
            const auto follower_only_pred = [follower_side](const Side& s) { return s == follower_side; };

            auto packet_ring_it = packet_rings_.find(follower_side);
            auto demuxer_it = demuxers_.find(follower_side);
            if (packet_ring_it == packet_rings_.end() || !packet_ring_it->second || demuxer_it == demuxers_.end() || !demuxer_it->second) {
              walk_skip_reason = "no-packet-ring";
            } else if (packet_ring_it->second->l1_disabled()) {
              walk_skip_reason = "l1-disabled";
            } else if (single_decoder_mode_.enabled()) {
              walk_skip_reason = "single-decoder";
            } else if (media_frame_detection_states_.at(follower_side).cardinality.load(std::memory_order_relaxed) == MediaFrameCardinality::SingleFrame) {
              walk_skip_reason = "single-frame-media";
            } else {
              PacketRing& packet_ring = *packet_ring_it->second;
              Demuxer& demuxer = *demuxer_it->second;
              const AVRational stream_tb = demuxer.time_base();
              const int64_t demuxer_start_us = demuxer.start_time();
              // Back up a little before window_start to give the decoder room
              // to produce clean output by the time we cross into the window.
              // (Decoders can emit garbage for the first fraction of a GOP.)
              constexpr int64_t kDecoderWarmupUs = 500000;  // 0.5 s
              const int64_t lookup_start_us = std::max<int64_t>(window_start_pts + demuxer_start_us - kDecoderWarmupUs, int64_t{0});
              const int64_t lookup_start_raw = av_rescale_q(lookup_start_us, AV_TIME_BASE_Q, stream_tb);
              {
                const auto walk_t_start = std::chrono::steady_clock::now();
                VideoDecoder& dec = *video_decoders_.at(follower_side);

                // Enter barrier scoped to the follower side; master pipeline keeps running.
                enter_seek_barrier(follower_only_pred);

                try {
                  // Pre-fill: when the packet ring doesn't yet span the walk
                  // window (typical under --start-paused, where the pipeline
                  // never streamed past its ~queue-depth of initial packets;
                  // also happens after a previous walk's post-seek restart
                  // fragmented the ring into overlapping ranges), clear the
                  // ring and refill it from a backward seek. The result is
                  // one contiguous range spanning [kf-before-window_start,
                  // window_end+], which the walk can iterate in one pass.
                  // Dropping the prior ranges is safe here: candidates we
                  // had already fingerprinted from them live in the retry
                  // cache (or the auto-align candidates vector for retry=0),
                  // and future L1 work will refill the ring as playback
                  // resumes via the post-walk restore.
                  const int64_t window_end_raw = av_rescale_q(window_end_pts + demuxer_start_us, AV_TIME_BASE_Q, stream_tb);
                  const auto ring_stats_pre = packet_ring.stats();
                  const bool need_prefill = ring_stats_pre.pts_max == INT64_MIN || ring_stats_pre.pts_max < window_end_raw || ring_stats_pre.range_count > 1;
                  if (need_prefill) {
                    packet_ring.clear();
                    const double seek_target_sec = static_cast<double>(lookup_start_raw) * av_q2d(stream_tb);
                    demuxer.seek(static_cast<float>(seek_target_sec), true);

                    int fill_appended = 0;
                    while (true) {
                      AVPacketUniquePtr packet{new AVPacket, avpacket_deleter};
                      av_init_packet(packet.get());
                      packet->data = nullptr;
                      if (!demuxer(*packet)) {
                        break;  // EOF
                      }
                      if (packet->stream_index != demuxer.video_stream_index()) {
                        continue;
                      }
                      const int64_t pkt_pts = packet->pts;
                      if (AVPacket* cloned = av_packet_clone(packet.get())) {
                        packet_ring.append(PacketRing::PacketPtr(cloned, avpacket_free_deleter));
                        ++fill_appended;
                      }
                      if (pkt_pts != AV_NOPTS_VALUE && pkt_pts > window_end_raw) {
                        break;
                      }
                    }
                    if (log_auto_align) {
                      const auto ring_stats_post = packet_ring.stats();
                      std::cerr << "[auto-align-prefill]"
                                << " pre_ranges=" << ring_stats_pre.range_count
                                << " pre_pts_max=" << ring_stats_pre.pts_max
                                << " appended=" << fill_appended
                                << " post_pts_max=" << ring_stats_post.pts_max
                                << std::endl;
                    }
                  }

                  // Find the walk's starting keyframe now (after pre-fill, which
                  // may have added new keyframes).
                  auto kf_hit = packet_ring.keyframe_at_or_before(lookup_start_raw);
                  if (!kf_hit.has_value()) {
                    walk_skip_reason = "no-keyframe-hit";
                  }
                  // Rebuild the filter graph so the post-walk pipeline restart
                  // can feed it again. The worker called close_src() during
                  // the barrier.
                  video_filterers_.at(follower_side)->reinit();

                  dec.flush();
                  dec.reset_pts_state();

                  // Pump one decoded raw frame: HW-transfer if needed,
                  // convert pts to AV_TIME_BASE μs, fingerprint if inside the
                  // window and not already ring-resident. Return true to stop
                  // (past window_end), false to keep going.
                  const auto handle_decoded = [&](AVFrame* raw) -> bool {
                    AVFrameUniquePtr sw_frame{nullptr, avframe_deleter};
                    AVFrame* usable = raw;
                    if (raw->format == dec.hw_pixel_format()) {
                      sw_frame = AVFrameUniquePtr{av_frame_alloc(), avframe_deleter};
                      if (sw_frame == nullptr) {
                        return false;
                      }
                      if (av_hwframe_transfer_data(sw_frame.get(), raw, 0) < 0) {
                        return false;
                      }
                      if (av_frame_copy_props(sw_frame.get(), raw) < 0) {
                        return false;
                      }
                      usable = sw_frame.get();
                    }
                    // raw->pts is in stream time_base. Convert to AV_TIME_BASE
                    // μs since demuxer start_time (the same unit ring frames use).
                    const int64_t frame_pts_us = av_rescale_q(usable->pts, stream_tb, AV_TIME_BASE_Q) - demuxer_start_us;
                    if (frame_pts_us > window_end_pts) {
                      return true;  // past window end; stop
                    }
                    if (frame_pts_us < window_start_pts) {
                      return false;  // before window; keep decoding forward
                    }
                    if (already_fingerprinted_pts.count(frame_pts_us) > 0) {
                      return false;  // already fingerprinted from ring or cache
                    }
                    Candidate c;
                    c.pts = frame_pts_us;
                    SwsContext* const ctx = get_or_build_sws(static_cast<AVPixelFormat>(usable->format), usable->width, usable->height);
                    if (ctx == nullptr) {
                      return false;
                    }
                    // swscale reads Y from the YUV source and outputs GRAY8.
                    // For packed formats (rare post-decode) it also works.
                    MetricsCalculator::compute_structural_fingerprint(usable->data, usable->linesize, usable->height, ctx, c.fp);
                    if (c.fp.empty()) {
                      return false;
                    }
                    candidates.push_back(std::move(c));
                    ++decoded_added;
                    return false;
                  };

                  // Feed packets from keyframe forward, pumping receive() until EAGAIN.
                  // Skipped if pre-fill couldn't produce a usable starting keyframe.
                  bool stop = !kf_hit.has_value();
                  if (kf_hit.has_value()) {
                  kf_hit->range->iterate_from(kf_hit->absolute_buffer_index, [&](const AVPacket* src_pkt) -> bool {
                    if (stop) {
                      return false;
                    }
                    AVPacket* cloned = av_packet_clone(src_pkt);
                    if (cloned == nullptr) {
                      return false;
                    }
                    dec.send(cloned);
                    av_packet_free(&cloned);
                    while (!stop) {
                      AVFrame* raw = av_frame_alloc();
                      if (raw == nullptr) {
                        return false;
                      }
                      if (!dec.receive(raw, &demuxer)) {
                        av_frame_free(&raw);
                        break;
                      }
                      if (handle_decoded(raw)) {
                        stop = true;
                      }
                      av_frame_free(&raw);
                    }
                    return !stop;
                  });
                  }  // end if (kf_hit.has_value())

                  // Drain decoder's B-frame-reorder buffer.
                  if (!stop) {
                    dec.send(nullptr);
                    while (!stop) {
                      AVFrame* raw = av_frame_alloc();
                      if (raw == nullptr) {
                        break;
                      }
                      if (!dec.receive(raw, &demuxer)) {
                        av_frame_free(&raw);
                        break;
                      }
                      if (handle_decoded(raw)) {
                        stop = true;
                      }
                      av_frame_free(&raw);
                    }
                  }
                } catch (const std::exception& ex) {
                  walk_skip_reason = "walk-exception";
                  std::cerr << "[auto-align-walk-exception] " << ex.what() << std::endl;
                } catch (...) {
                  walk_skip_reason = "walk-exception";
                  std::cerr << "[auto-align-walk-exception] unknown" << std::endl;
                }

                // Post-walk restore: flush decoder state, seek demuxer back to
                // the keyframe at or before follower.current so the worker
                // produces contiguous frames starting at (or just before) the
                // current ring position. Using the forward-first flag lands on
                // the NEXT keyframe (potentially seconds ahead for sparse-key
                // encodes), which then pollutes the ring prefetch with
                // far-forward frames and turns a subsequent L0-pivot into a
                // wild jump. Seek backward-first; drop the forward fallback.
                dec.flush();
                dec.reset_pts_state();
                const float follower_start_sec = static_cast<float>(demuxer_start_us) * static_cast<float>(AV_TIME_TO_SEC);
                const float resume_sec = static_cast<float>(follower_current->pts) * static_cast<float>(AV_TIME_TO_SEC) + follower_start_sec + 0.001f;
                demuxer.seek(resume_sec, true);
                for (auto& p : packet_queues_) {
                  if (follower_only_pred(p.first)) {
                    p.second->restart();
                  }
                }
                for (auto& p : decoded_frame_queues_) {
                  if (follower_only_pred(p.first)) {
                    p.second->restart();
                  }
                }
                for (auto& p : filtered_frame_queues_) {
                  if (follower_only_pred(p.first)) {
                    p.second->restart();
                  }
                }
                for (auto& p : converted_frame_queues_) {
                  if (follower_only_pred(p.first)) {
                    p.second->restart();
                  }
                }
                decode_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - walk_t_start).count());
              }
            }
            if (log_auto_align && walk_skip_reason != nullptr) {
              std::cerr << "[auto-align-walk-skip] reason=" << walk_skip_reason << std::endl;
            }
          }

          // --- Build the master-probe fingerprint cache ---
          // Probes sample at k · probe_step_pts around master.current on its
          // own axis. probe_step is the coarser of the two sides' delta_pts
          // values so each probe maps to a distinct frame on both sides.
          const int64_t probe_step_pts = std::max(master_delta_pts, follower_delta_pts);
          const int64_t probe_tolerance = probe_step_pts / 2;
          std::unordered_map<int64_t, std::vector<float>> master_probe_fps;
          // Reuse probe fingerprints from the previous press when the cache is
          // valid: master hasn't moved, so the probe frames and their PTS keys
          // are identical. Probe radius is constant, so no new probes appear
          // under retry.
          if (can_reuse_cache) {
            master_probe_fps = std::move(retry.master_probe_fingerprints);
          } else {
            for (int k = -kAutoAlignProbeRadius; k <= kAutoAlignProbeRadius; ++k) {
              const int64_t t_k = master_current_pts + static_cast<int64_t>(k) * probe_step_pts;
              const AVFrame* const probe_frame = find_nearest_in_ring(master_state.ring, t_k, probe_tolerance);
              if (probe_frame == nullptr) {
                continue;
              }
              if (master_probe_fps.find(probe_frame->pts) != master_probe_fps.end()) {
                continue;
              }
              std::vector<float> fp;
              if (!fingerprint_ring_frame(probe_frame, fp)) {
                continue;
              }
              master_probe_fps.emplace(probe_frame->pts, std::move(fp));
            }
          }

          // Find the nearest candidate (by PTS) to a target on the follower
          // time axis. Returns nullptr if no candidate is within tolerance_pts.
          const auto find_nearest_candidate = [&](int64_t target_pts, int64_t tolerance_pts) -> const Candidate* {
            const Candidate* best = nullptr;
            int64_t best_dist = std::numeric_limits<int64_t>::max();
            for (const auto& c : candidates) {
              const int64_t dist = std::abs(c.pts - target_pts);
              if (dist < best_dist) {
                best_dist = dist;
                best = &c;
              }
            }
            if (best == nullptr || best_dist > tolerance_pts) {
              return nullptr;
            }
            return best;
          };

          // --- Score each candidate with windowed multi-probe correlation ---
          // Scores are PTS-keyed so the decision phase below can look up an
          // arbitrary candidate (e.g. follower_current, or a tie-break peer).
          struct ScoredCandidate {
            int64_t pts{0};
            float score{0.0f};
            int n_valid{0};
          };
          std::vector<ScoredCandidate> scored;
          scored.reserve(candidates.size());
          int valid_scored = 0;

          for (const auto& cand : candidates) {
            const int64_t delta_t_hypothesis = cand.pts - master_current_pts;
            float sum = 0.0f;
            int n_valid = 0;
            for (int k = -kAutoAlignProbeRadius; k <= kAutoAlignProbeRadius; ++k) {
              const int64_t m_target = master_current_pts + static_cast<int64_t>(k) * probe_step_pts;
              const AVFrame* const m_probe = find_nearest_in_ring(master_state.ring, m_target, probe_tolerance);
              if (m_probe == nullptr) {
                continue;
              }
              const auto m_it = master_probe_fps.find(m_probe->pts);
              if (m_it == master_probe_fps.end()) {
                continue;
              }
              const int64_t f_target = m_target + delta_t_hypothesis;
              const Candidate* const f_probe = find_nearest_candidate(f_target, probe_tolerance);
              if (f_probe == nullptr) {
                continue;
              }
              sum += MetricsCalculator::structural_correlation(m_it->second, f_probe->fp);
              ++n_valid;
            }
            if (n_valid < 3) {
              continue;  // under-supported hypothesis; disregard
            }
            const float score = sum / static_cast<float>(n_valid);
            ++valid_scored;
            scored.push_back({cand.pts, score, n_valid});
            if (log_auto_align) {
              std::cerr << "[auto-align]"
                        << " pts_delta_ms=" << string_sprintf("%.3f", static_cast<double>(cand.pts - master_current_pts) / 1000.0)
                        << " probes=" << n_valid << "/" << (2 * kAutoAlignProbeRadius + 1)
                        << " score=" << string_sprintf("%.4f", score)
                        << std::endl;
            }
          }

          // Look up the score of a specific candidate by PTS. Linear over
          // `scored` — small list, called a handful of times per press.
          const auto score_of_pts = [&](int64_t pts, float* out) -> bool {
            for (const auto& s : scored) {
              if (s.pts == pts) {
                *out = s.score;
                return true;
              }
            }
            return false;
          };

          // Resolve follower_current's own score if we managed to score it.
          // (We always fingerprint ring.at(0) = current above, so this should
          // hold unless probe_step or ring depth made probes undiscoverable.)
          float current_score = -std::numeric_limits<float>::max();
          const bool current_scored = score_of_pts(follower_current->pts, &current_score);

          // --- Decide ---
          // Human-readable searched-interval descriptor on the follower axis
          // (relative to follower_current). Symmetric "±N.Ns"; directional
          // "+N.Ns" or "-N.Ns".
          const int64_t searched_low_rel_pts = searched_low_pts - follower_current_pts;
          const int64_t searched_high_rel_pts = searched_high_pts - follower_current_pts;
          const auto format_searched_interval = [&]() -> std::string {
            const double low_s = static_cast<double>(searched_low_rel_pts) * static_cast<double>(AV_TIME_TO_SEC);
            const double high_s = static_cast<double>(searched_high_rel_pts) * static_cast<double>(AV_TIME_TO_SEC);
            if (auto_align_mode == AutoAlignMode::Symmetric) {
              const double max_half = std::max(-low_s, high_s);
              return string_sprintf("+/-%.1fs", max_half);
            } else if (auto_align_mode == AutoAlignMode::Forward) {
              return string_sprintf("+%.1fs", high_s);
            } else {
              return string_sprintf("-%.1fs", -low_s);
            }
          };

          // Clip-boundary saturation signals that this press can't do new work
          // in the pressed direction. Used to override "nothing found" outcomes
          // with a directional boundary message, and to block ` when both ends
          // are saturated.
          const bool sym_fully_saturated = (auto_align_mode == AutoAlignMode::Symmetric) && low_saturated && high_saturated && !new_work_low && !new_work_high;
          const bool back_saturated = (auto_align_mode == AutoAlignMode::Backward) && low_saturated && !new_work_low;
          const bool fwd_saturated = (auto_align_mode == AutoAlignMode::Forward) && high_saturated && !new_work_high;

          // Mode-specific candidate filtering / selection. For `, pick only
          // candidates with score strictly greater than current + eps, with
          // distance + "ahead preferred over behind" tie-breaks. For [ / ],
          // pick in-direction candidates with score >= current - eps
          // (excluding current itself), with nearest-in-direction tie-break.
          const auto pick_symmetric_best = [&]() -> const ScoredCandidate* {
            if (!current_scored) {
              return nullptr;
            }
            // Eligibility gate: strictly stronger than current by improvement
            // eps. Tie-break between eligible candidates uses the much tighter
            // tie-break band so a sharp peak beats a nearby near-peer.
            const float threshold = current_score + kAutoAlignImprovementEps;
            const ScoredCandidate* best = nullptr;
            for (const auto& s : scored) {
              if (s.score <= threshold) {
                continue;
              }
              if (best == nullptr) {
                best = &s;
                continue;
              }
              const bool strictly_stronger = s.score > best->score + kAutoAlignTieBreakBand;
              const bool score_tied = std::abs(s.score - best->score) <= kAutoAlignTieBreakBand;
              if (strictly_stronger) {
                best = &s;
                continue;
              }
              if (!score_tied) {
                continue;
              }
              const int64_t s_dist = std::abs(s.pts - follower_current->pts);
              const int64_t best_dist = std::abs(best->pts - follower_current->pts);
              if (s_dist < best_dist) {
                best = &s;
                continue;
              }
              if (s_dist == best_dist && s.pts > follower_current->pts && best->pts <= follower_current->pts) {
                best = &s;
              }
            }
            return best;
          };
          const auto pick_directional_best = [&](bool forward) -> const ScoredCandidate* {
            if (!current_scored) {
              return nullptr;
            }
            // Eligibility gate: at-or-above current by improvement eps, in the
            // pressed direction, excluding current itself. Tie-break between
            // eligible candidates uses the tight tie-break band so peaks win
            // cleanly; a 1.0 short-circuit covers the case where a near-
            // perfect match should be chosen regardless of distance.
            const float threshold = current_score - kAutoAlignImprovementEps;
            const ScoredCandidate* best = nullptr;
            for (const auto& s : scored) {
              if (s.pts == follower_current->pts) {
                continue;
              }
              if (forward && s.pts <= follower_current->pts) {
                continue;
              }
              if (!forward && s.pts >= follower_current->pts) {
                continue;
              }
              if (s.score < threshold) {
                continue;
              }
              const bool s_near_one = s.score >= 1.0f - kAutoAlignShortCircuitEps;
              const bool best_near_one = (best != nullptr) && best->score >= 1.0f - kAutoAlignShortCircuitEps;
              if (best == nullptr) {
                best = &s;
                continue;
              }
              if (s_near_one && !best_near_one) {
                best = &s;
                continue;
              }
              if (!s_near_one && best_near_one) {
                continue;
              }
              const bool strictly_stronger = s.score > best->score + kAutoAlignTieBreakBand;
              const bool score_tied = std::abs(s.score - best->score) <= kAutoAlignTieBreakBand;
              if (strictly_stronger) {
                best = &s;
                continue;
              }
              if (!score_tied) {
                continue;
              }
              const int64_t s_dist = std::abs(s.pts - follower_current->pts);
              const int64_t best_dist = std::abs(best->pts - follower_current->pts);
              if (s_dist < best_dist) {
                best = &s;
              }
            }
            return best;
          };

          const ScoredCandidate* picked = nullptr;
          if (auto_align_mode == AutoAlignMode::Symmetric) {
            picked = pick_symmetric_best();
          } else if (auto_align_mode == AutoAlignMode::Forward) {
            picked = pick_directional_best(/*forward=*/true);
          } else {
            picked = pick_directional_best(/*forward=*/false);
          }

          // Find the best unconstrained score in the pool — used as a floor
          // check: if nothing in the whole pool meets the confidence floor,
          // this press's search region contained no reliable alignment
          // candidates and the user should expand further.
          float pool_best_score = -std::numeric_limits<float>::max();
          int64_t pool_best_pts = follower_current->pts;
          for (const auto& s : scored) {
            if (s.score > pool_best_score) {
              pool_best_score = s.score;
              pool_best_pts = s.pts;
            }
          }

          std::string decision_kind;
          int shift_applied = 0;
          int64_t best_pts = follower_current->pts;
          float best_score = current_scored ? current_score : -std::numeric_limits<float>::max();
          if (valid_scored == 0) {
            // Boundary saturation with no scoreable candidates: report the
            // boundary rather than generic "insufficient probes" so the user
            // knows why.
            if (back_saturated) {
              display_->set_pending_message("Auto-align: reached clip start");
              decision_kind = "boundary_start";
            } else if (fwd_saturated) {
              display_->set_pending_message("Auto-align: reached clip end");
              decision_kind = "boundary_end";
            } else if (sym_fully_saturated) {
              display_->set_pending_message("Auto-align: search exhausted (both clip boundaries reached)");
              decision_kind = "boundary_both";
            } else {
              display_->set_pending_message("Auto-align: insufficient probes — no change");
              decision_kind = "no_frames";
            }
          } else if (pool_best_score < kAutoAlignConfidenceFloor) {
            // Nothing in the pool meets the confidence floor. If the pressed
            // direction is saturated, expanding further won't help — report
            // the boundary. Otherwise prompt the user to expand.
            if (back_saturated) {
              display_->set_pending_message("Auto-align: reached clip start");
              decision_kind = "boundary_start";
            } else if (fwd_saturated) {
              display_->set_pending_message("Auto-align: reached clip end");
              decision_kind = "boundary_end";
            } else if (sym_fully_saturated) {
              display_->set_pending_message(string_sprintf("Auto-align: search exhausted (both clip boundaries reached, best score %.3f)", pool_best_score));
              decision_kind = "boundary_both";
            } else if (!new_work_low && !new_work_high) {
              display_->set_pending_message(string_sprintf("Auto-align: still low confidence (score %.3f, window %s)", pool_best_score, format_searched_interval().c_str()));
              decision_kind = "low_confidence";
            } else {
              display_->set_pending_message(string_sprintf("Auto-align: low confidence (score %.3f) — press again to extend", pool_best_score));
              decision_kind = "low_confidence";
            }
            best_score = pool_best_score;
            best_pts = pool_best_pts;
          } else if (picked == nullptr) {
            // We have at least one confident candidate, but nothing satisfies
            // the mode's pick rule. Distinguish boundary vs genuine no-op.
            if (auto_align_mode == AutoAlignMode::Symmetric) {
              if (sym_fully_saturated) {
                display_->set_pending_message(string_sprintf("Auto-align: search exhausted (both clip boundaries reached, score %.3f)", pool_best_score));
                decision_kind = "boundary_both";
              } else {
                display_->set_pending_message(string_sprintf("Auto-align: already aligned (score %.3f)", current_scored ? current_score : pool_best_score));
                decision_kind = "already";
              }
            } else if (auto_align_mode == AutoAlignMode::Forward) {
              if (fwd_saturated) {
                display_->set_pending_message("Auto-align: reached clip end");
                decision_kind = "boundary_end";
              } else {
                display_->set_pending_message(string_sprintf("Auto-align: no stronger match ahead (score %.3f)", current_scored ? current_score : pool_best_score));
                decision_kind = "no_stronger_in_direction";
              }
            } else {
              if (back_saturated) {
                display_->set_pending_message("Auto-align: reached clip start");
                decision_kind = "boundary_start";
              } else {
                display_->set_pending_message(string_sprintf("Auto-align: no stronger match behind (score %.3f)", current_scored ? current_score : pool_best_score));
                decision_kind = "no_stronger_in_direction";
              }
            }
            best_score = pool_best_score;
            best_pts = pool_best_pts;
          } else {
            best_pts = picked->pts;
            best_score = picked->score;
            const int64_t shift_pts = best_pts - follower_current->pts;
            shift_applied = static_cast<int>(std::llround(static_cast<double>(shift_pts) / static_cast<double>(follower_delta_pts)));

            // Dispatch splits on follower side. Follower == RIGHT (no swap)
            // folds into the shift_right_frames pathway so L0 pivot / L1
            // re-decode can handle the common case with no extra seek work.
            // Follower == LEFT (swap) can't use that pathway — shift_right_
            // frames is denominated in RIGHT-side frames and L0 pivot is
            // right-only — so we hand off to the absolute-seek path via the
            // same plumbing shift-click uses (Phase 2), which scopes the
            // seek to LEFT through should_seek(follower_side).
            if (auto_align_follower_side.is_right()) {
              shift_right_frames += shift_applied;
            } else {
              const double follower_start_sec = static_cast<double>(follower_state.start_time_);
              const double target_abs_sec = static_cast<double>(best_pts) * static_cast<double>(AV_TIME_TO_SEC) + follower_start_sec;
              const double duration = (shortest_duration_ > 0.0) ? shortest_duration_ : 1.0;
              const double fractional = (target_abs_sec - follower_start_sec) / duration;
              seek_relative = static_cast<float>(fractional);
              seek_from_start = true;
              display_->set_right_only_seek(true, auto_align_follower_side);
            }

            // Stash everything the post-seek verification step needs. Master
            // probes don't move during the seek, so their PTS keys remain
            // valid references into master_state.ring.
            pending_auto_align_verification_ = {};
            pending_auto_align_verification_.active = true;
            pending_auto_align_verification_.expected_score = best_score;
            pending_auto_align_verification_.expected_shift_frames = shift_applied;
            pending_auto_align_verification_.follower_side = auto_align_follower_side;
            pending_auto_align_verification_.master_current_pts = master_current_pts;
            pending_auto_align_verification_.probe_step_pts = probe_step_pts;
            pending_auto_align_verification_.delta_t_pts = best_pts - master_current_pts;
            // Clone (not move) the master fingerprints so the cache update
            // below can also preserve them across this seek for subsequent
            // directional-iteration presses.
            pending_auto_align_verification_.master_probe_fingerprints = master_probe_fps;
            display_->set_pending_message(string_sprintf("Auto-align: shift %+d frame%s (score %.3f)", shift_applied, std::abs(shift_applied) == 1 ? "" : "s", best_score));
            decision_kind = "seek";
          }
          if (log_auto_align) {
            // When no candidate got scored, best_score/current_score are still
            // the -max float sentinel; print "n/a" instead of a 40-digit number.
            const std::string best_score_s = (valid_scored == 0) ? std::string{"n/a"} : string_sprintf("%.4f", best_score);
            const std::string current_score_s = current_scored ? string_sprintf("%.4f", current_score) : std::string{"n/a"};
            const char* walk_state = walk_attempted ? (walk_skip_reason ? "skipped" : "done") : "none";
            std::cerr << "[auto-align]"
                      << " mode=" << mode_label
                      << " follower=" << auto_align_follower_side.to_string()
                      << " ring=" << ring_contributed
                      << " decoded=" << decoded_added
                      << " decode_ms=" << decode_ms
                      << " walk=" << walk_state
                      << " candidates=" << candidates.size()
                      << " scored=" << valid_scored
                      << " best_pts_delta_ms=" << string_sprintf("%.3f", static_cast<double>(best_pts - master_current_pts) / 1000.0)
                      << " best_score=" << best_score_s
                      << " current_score=" << current_score_s
                      << " shift_frames=" << shift_applied
                      << " decision=" << decision_kind
                      << " searched_window=[" << string_sprintf("%.3f", static_cast<double>(searched_low_rel_pts) * static_cast<double>(AV_TIME_TO_SEC))
                      << "," << string_sprintf("%.3f", static_cast<double>(searched_high_rel_pts) * static_cast<double>(AV_TIME_TO_SEC)) << "]s"
                      << " low_sat=" << (low_saturated ? 1 : 0)
                      << " high_sat=" << (high_saturated ? 1 : 0)
                      << std::endl;
          }

          // --- Update retry cache ---
          // Persist on every successful pass through the decide branch so the
          // next press can extend the searched interval regardless of which
          // key was pressed. Invalidation is handled at the TOP of the next
          // press (master/follower PTS mismatch, side swap).
          retry.valid = true;
          retry.follower_side = auto_align_follower_side;
          retry.master_pts_at_cache = master_current->pts;
          // On a seek the follower is about to land at best_pts; the next
          // press will compare its master/follower PTS against this to
          // validate cache freshness. Post-seek verification will fix up
          // follower_pts_at_cache to the exact landed PTS.
          retry.follower_pts_at_cache = (decision_kind == "seek") ? best_pts : follower_current->pts;
          retry.searched_low_pts = searched_low_pts;
          retry.searched_high_pts = searched_high_pts;
          retry.low_saturated = low_saturated;
          retry.high_saturated = high_saturated;
          retry.packet_buffer_miss = (walk_attempted && walk_skip_reason != nullptr);
          retry.candidates = std::move(candidates);
          retry.master_probe_fingerprints = std::move(master_probe_fps);
          }  // end of cache/decode/score/decide block
        }
      }

      // if seeking is required, drain packet and frame queues
      if ((seek_relative != 0.0F) || (shift_right_frames != 0) || force_seek_current_position) {
        const auto seek_t_start = std::chrono::steady_clock::now();
        const char* seek_tier = "L2";  // default; pivot / L1 paths override below
        int64_t seek_target_pts_log = AV_NOPTS_VALUE;  // populated by L1 when known
        auto log_seek = [&](const char* override_tier = nullptr) {
          if (!log_seek_timing) {
            return;
          }
          const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - seek_t_start).count();
          std::cerr << "[seek-timing]"
                    << " tier=" << (override_tier ? override_tier : seek_tier)
                    << " shift_right_frames=" << shift_right_frames
                    << " seek_relative=" << seek_relative
                    << " target_pts=" << seek_target_pts_log
                    << " elapsed_us=" << elapsed_us
                    << std::endl;
        };
        // Any activity entering the seek block permanently disables sticky single-
        // decoder mode: once the right side has diverged from the left (in PTS, crop,
        // or filter), it can't silently re-converge without the user explicitly resetting
        // state. Removing the dynamic re-enable path removes the single-decoder boundary
        // transitions that used to deadlock the pivot fast paths.
        single_decoder_mode_.disable_sticky();

        // update total right time shifted and recompute the static shift in TimeShifter
        if (shift_right_frames != 0) {
          total_right_time_shifted += shift_right_frames;
        }
        time_shifter_.set_frame_shift_accumulator(total_right_time_shifted, right_delta);

        // A "pure right frame shift" is a `+`/`-` keypress with no scrub and no crop
        // pending. In that case the left side's position does not change, so we skip
        // flushing/seeking its pipeline entirely. Only right sides participate. Note
        // that the sticky disable above guarantees single-decoder mode is off here, so
        // there is no boundary-crossing risk.
        const bool pure_right_frame_shift = (seek_relative == 0.0F) && (shift_right_frames != 0) && !force_seek_current_position;
        // Right-only scoping applies when the user explicitly opted in via
        // shift-click (`display_->get_right_only_seek()`), or when this is a
        // pure `+`/`-` frame shift (which has always been right-only). Forces
        // the full-seek barrier and demuxer work to touch only one side's
        // pipeline; the other side keeps playing.
        const bool right_only_seek = pure_right_frame_shift || (display_->get_right_only_seek() && !force_seek_current_position);
        // Which side actually participates in this right-only seek. Normally
        // RIGHT; becomes LEFT when the user shift-clicked while `swap_left_right_`
        // was active — shift-click follows the visual position of the "right"
        // side, not the underlying pipeline identity. Pure `+`/`-` shifts
        // always target RIGHT (their sign already flipped at the input layer
        // to preserve the swap illusion; see Phase 1).
        const Side follower_side =
            (display_->get_right_only_seek() && !pure_right_frame_shift)
                ? display_->get_right_only_seek_follower()
                : RIGHT;
        const Side master_side = follower_side.is_left() ? RIGHT : LEFT;

        // Fast path: pivot the FrameRing cursor instead of seeking.
        //
        // Each right side's `FrameRing` holds up to `frame_buffer_size_` frames of
        // history and the same of prefetch around a single `current` slot. The prefetch
        // is replenished each iteration from `converted_frame_queues_`, so `+N` pivots
        // extend to the full prefetch capacity — not just the raw converter queue
        // depth. Backward pivots preserve the stepped-over frames in prefetch, so a
        // subsequent forward pivot replays them without re-decoding.
        //
        // `pure_right_frame_shift` already excludes cases where a pivot would leave the
        // pipeline in an inconsistent state (sticky single-decoder mode off, no crop
        // change, no scrub). Either direction: no pipeline drain, no `ReadyToSeek`
        // barrier, no filter reinit, no demuxer seek, no re-decode.
        bool backward_pivot_possible = pure_right_frame_shift && shift_right_frames < 0;
        bool forward_pivot_possible = pure_right_frame_shift && shift_right_frames > 0;

        if (backward_pivot_possible) {
          const size_t n = static_cast<size_t>(-shift_right_frames);
          for (const auto& pair : side_states) {
            if (!pair.first.is_right()) {
              continue;
            }
            const SideState& right_state = pair.second;
            if (static_cast<size_t>(n) > static_cast<size_t>(right_state.ring.history_size()) || right_state.delta_pts_ <= 0) {
              backward_pivot_possible = false;
              break;
            }
          }
        }

        if (forward_pivot_possible) {
          const size_t n = static_cast<size_t>(shift_right_frames);
          for (const auto& pair : side_states) {
            if (!pair.first.is_right()) {
              continue;
            }
            const SideState& right_state = pair.second;
            if (right_state.delta_pts_ <= 0) {
              forward_pivot_possible = false;
              break;
            }
            // Pre-check: every right side's prefetch must already hold N frames. The
            // prefetch is topped up from `converted_frame_queues_` at the top of every
            // iteration, so forward `+N` pivots scale up to `prefetch_capacity`, not
            // just the raw converter queue depth.
            if (static_cast<size_t>(right_state.ring.prefetch_size()) < n) {
              forward_pivot_possible = false;
              break;
            }
          }
        }

        if (backward_pivot_possible) {
          seek_tier = "L0back";
          const int n = -shift_right_frames;
          for (auto& pair : side_states) {
            if (!pair.first.is_right()) {
              continue;
            }
            SideState& right_state = pair.second;

            // Pivot the cursor n steps back. `current` + front-of-history slide into
            // prefetch (so a subsequent `+N` restores them without re-decoding), and
            // `current` is set to what used to be `history[n-1]`.
            right_state.ring.pivot_backward(n);

            const AVFrame* new_current = right_state.ring.current_frame();

            right_state.effective_time_shift_ = time_shifter_.effective_shift(new_current->pts);
            right_state.pts_ = new_current->pts - right_state.effective_time_shift_;
            right_state.previous_decoded_picture_number_ = -1;
            right_state.decoded_picture_number_ = 1;
          }

          // Don't sync until the next iteration (consistent with the full-seek path).
          skip_update = true;
        } else if (forward_pivot_possible) {
          seek_tier = "L0forward";
          const int n = shift_right_frames;
          for (auto& pair : side_states) {
            if (!pair.first.is_right()) {
              continue;
            }
            SideState& right_state = pair.second;

            // Pivot the cursor n steps forward. `current` + n-1 front-of-prefetch
            // entries slide into history (so a subsequent `-N` can restore them).
            right_state.ring.pivot_forward(n);

            const AVFrame* new_current = right_state.ring.current_frame();

            right_state.effective_time_shift_ = time_shifter_.effective_shift(new_current->pts);
            right_state.pts_ = new_current->pts - right_state.effective_time_shift_;
            right_state.previous_decoded_picture_number_ = -1;
            right_state.decoded_picture_number_ = 1;
          }

          // Don't sync until the next iteration (consistent with the full-seek path).
          skip_update = true;
        } else {
          // Predicate: does this side participate in this seek? When
          // `right_only_seek` is true, only the follower side (RIGHT without
          // swap, LEFT under swap-aware shift-click) seeks. Otherwise all
          // sides participate (standard full seek).
          const auto should_seek = [right_only_seek, follower_side](const Side& s) -> bool {
            if (!right_only_seek) {
              return true;
            }
            return follower_side.is_left() ? s.is_left() : s.is_right();
          };

          // =====================================================================
          // L1 re-decode fast path.
          //
          // When the user's seek target is close enough to the nearest keyframe
          // in our PacketRing, we can drive the decoder(s) inline from the
          // keyframe instead of a full L2 pipeline drain. Scope includes:
          //   - pure right frame shift (`-` / `+`), either direction
          //   - relative seeks (Shift+A/D, arrow keys, PageUp/Down)
          //   - absolute seeks (paused timeline click)
          //
          // Eligibility gates below fall back to L2 if any fail.
          //   - force_seek_current_position (crop/HDR/filter reconfig): L2 handles
          //     dimension recalc and format-converter recreation; L1 stays simpler.
          //   - single_decoder_mode: shared decoder between sides is incompatible
          //     with independent L1 per-side flushes.
          //   - GOP heuristic (VIDEO_COMPARE_L1_MAX_KF_DISTANCE_SEC, default 0.5s):
          //     keeps L1 off long-GOP content where L2's parallel pipeline wins.
          // =====================================================================
          bool l1_eligible = !force_seek_current_position && !single_decoder_mode_.enabled();
          const char* l1_skip_reason = force_seek_current_position ? "force-seek-current-position" : (single_decoder_mode_.enabled() ? "single-decoder" : nullptr);

          std::map<Side, int64_t> l1_target_pts;         // raw demuxer-time_base PTS (for PacketRing lookup)
          std::map<Side, int64_t> l1_target_frame_pts;   // AV_TIME_BASE microseconds-since-start (for decoded-frame comparison)
          std::map<Side, float> l1_target_sec;           // seconds (for post-L1 demuxer reseek)

          // Compute per-side target position (seconds in the side's own axis).
          // Scoped so the locals don't collide with L2's target-calc below
          // when we fall through.
          float l1_next_left_position_cache = 0.0F;  // valid iff l1_eligible && should_seek(LEFT)
          auto compute_side_target_sec = [&](const Side& side, const SideState& ss) -> float {
            if (side.is_left()) return l1_next_left_position_cache;
            const float right_position = left.pts_ * AV_TIME_TO_SEC + ss.start_time_;
            const bool right_is_single_frame = media_frame_detection_states_.at(side).cardinality.load(std::memory_order_relaxed) == MediaFrameCardinality::SingleFrame;
            float eff = seek_relative;
            if (right_is_single_frame && !seek_from_start) eff = ss.delta_pts_ * AV_TIME_TO_SEC;
            float pos = (seek_from_start && !right_is_single_frame) ? (shortest_duration_ * eff + ss.start_time_) : (right_position + eff);
            const float min_right_position = (ss.first_pts_ > INT64_MIN) ? (ss.first_pts_ * AV_TIME_TO_SEC + ss.start_time_) : ss.start_time_;
            pos = std::max(pos, min_right_position);
            pos += time_shifter_.static_shift() * AV_TIME_TO_SEC;
            pos += static_cast<float>(time_shifter_.dynamic_shift(static_cast<int64_t>((pos - ss.start_time_) / AV_TIME_TO_SEC), false)) * AV_TIME_TO_SEC;
            return pos;
          };
          // LEFT target (if participating). Mirrors L2's left calc.
          if (l1_eligible && should_seek(LEFT)) {
            const bool left_is_sf = media_frame_detection_states_.at(LEFT).cardinality.load(std::memory_order_relaxed) == MediaFrameCardinality::SingleFrame;
            const float min_left = (left.first_pts_ > INT64_MIN) ? (left.first_pts_ * AV_TIME_TO_SEC + left.start_time_) : left.start_time_;
            const float left_pos = left.pts_ * AV_TIME_TO_SEC + left.start_time_;
            float eff = seek_relative;
            if (left_is_sf && !seek_from_start) eff = left.delta_pts_ * AV_TIME_TO_SEC;
            float pos = (seek_from_start && !left_is_sf) ? (shortest_duration_ * eff + left.start_time_) : (left_pos + eff);
            l1_next_left_position_cache = std::max(pos, min_left);
          }

          // Per-side eligibility: all seeking sides must have a valid PacketRing
          // that covers the target and a keyframe close enough to it.
          if (l1_eligible) {
            for (auto& pair : side_states) {
              const Side& side = pair.first;
              if (!should_seek(side)) continue;
              SideState& ss = pair.second;

              const bool this_is_single_frame = media_frame_detection_states_.at(side).cardinality.load(std::memory_order_relaxed) == MediaFrameCardinality::SingleFrame;
              if (this_is_single_frame) {
                l1_eligible = false;
                l1_skip_reason = "single-frame-media";
                break;
              }

              auto ring_it = packet_rings_.find(side);
              if (ring_it == packet_rings_.end() || !ring_it->second) {
                l1_eligible = false;
                l1_skip_reason = "no-packet-ring";
                break;
              }
              if (ring_it->second->l1_disabled()) {
                l1_eligible = false;
                l1_skip_reason = "l1-disabled";
                break;
              }

              const float target_sec = compute_side_target_sec(side, ss);
              const AVRational stream_tb = demuxers_[side]->time_base();
              const int64_t target_in_av_time = static_cast<int64_t>(std::llround((static_cast<double>(target_sec) - ss.start_time_) * static_cast<double>(AV_TIME_BASE)));
              const int64_t target_raw_pts = av_rescale_q(target_in_av_time, AV_TIME_BASE_Q, stream_tb);

              if (!ring_it->second->covers(target_raw_pts)) {
                l1_eligible = false;
                l1_skip_reason = "target-not-covered";
                if (log_seek_timing) {
                  const auto s = ring_it->second->stats();
                  std::cerr << "[l1-skip]"
                            << " side=" << side.to_string()
                            << " target_pts=" << target_raw_pts
                            << " ring_pts=[" << s.pts_min << "," << s.pts_max << "]"
                            << " bytes=" << s.bytes_used
                            << " tb=" << stream_tb.num << "/" << stream_tb.den
                            << std::endl;
                }
                break;
              }

              // GOP heuristic.
              auto kf_hit = ring_it->second->keyframe_at_or_before(target_raw_pts);
              if (!kf_hit.has_value()) {
                l1_eligible = false;
                l1_skip_reason = "no-keyframe-hit";
                break;
              }
              const int64_t kf_distance_ticks = target_raw_pts - kf_hit->kf_pts;
              const double kf_distance_sec = static_cast<double>(kf_distance_ticks) * static_cast<double>(stream_tb.num) / static_cast<double>(stream_tb.den);
              if (kf_distance_sec > l1_max_kf_distance_sec) {
                l1_eligible = false;
                l1_skip_reason = "gop-too-long";
                if (log_seek_timing) {
                  std::cerr << "[l1-skip]"
                            << " side=" << side.to_string()
                            << " kf_distance=" << kf_distance_sec << "s"
                            << " > " << l1_max_kf_distance_sec << "s"
                            << std::endl;
                }
                break;
              }

              l1_target_pts[side] = target_raw_pts;
              l1_target_frame_pts[side] = target_in_av_time;
              l1_target_sec[side] = target_sec;
            }
          }
          if (log_seek_timing && !l1_eligible && l1_skip_reason) {
            std::cerr << "[l1-skip] reason=" << l1_skip_reason << std::endl;
          }

          if (l1_eligible) {
            // ==== L1 path ====
            seek_tier = "L1";
            // Log the target for the first seeking right side (there's at most
            // one active right at a time from the user's perspective).
            for (auto& p : l1_target_pts) {
              seek_target_pts_log = p.second;
              break;
            }
            if (log_l1_stages) std::cerr << "[l1-stage] enter" << std::endl;
            enter_seek_barrier(should_seek);
            if (log_l1_stages) std::cerr << "[l1-stage] barrier-idle" << std::endl;

            // Per-seeking-side: flush decoder, reinit filter graph, feed packets
            // from keyframe, capture the frame whose PTS >= target, seed
            // ring.history with the pre-target frames, set ring.current().
            //
            // FFmpeg send/receive/reinit can throw. Wrap so L1 fallback to L2
            // instead of tearing down the program.
            bool l1_ok = true;
            const char* l1_fail_reason = nullptr;
            try {
            // Filter graph was close_src'd during the barrier; consume any
            // pending filter-change request (so a pending crop snapshot applies)
            // and rebuild the graph so it accepts frames again.
            for (auto& pair : video_filterers_) {
              if (!should_seek(pair.first)) continue;
              pair.second->consume_filter_change();
              pair.second->reinit();
            }
            if (log_l1_stages) std::cerr << "[l1-stage] filter-reinit-done" << std::endl;
            for (auto& pair : side_states) {
              const Side& side = pair.first;
              if (!should_seek(side)) continue;
              SideState& side_state = pair.second;

              const int64_t target_pts = l1_target_pts[side];
              const int64_t target_frame_pts = l1_target_frame_pts[side];
              auto hit = packet_rings_[side]->keyframe_at_or_before(target_pts);
              if (!hit.has_value()) {
                l1_ok = false;
                l1_fail_reason = "no-keyframe-hit";
                break;
              }

              VideoDecoder& dec = *video_decoders_[side];
              VideoFilterer& flt = *video_filterers_[side];
              FormatConverter& cvt = *format_converters_[side];

              if (log_l1_stages) {
                std::cerr << "[l1-stage] side=" << side.to_string() << " before dec.flush" << std::endl;
              }
              dec.flush();
              dec.reset_pts_state();
              if (log_l1_stages) {
                std::cerr << "[l1-stage]"
                          << " side=" << side.to_string()
                          << " after dec.flush"
                          << ", kf_pts=" << hit->kf_pts
                          << " target_pts=" << target_pts
                          << " kf_idx=" << hit->absolute_buffer_index
                          << std::endl;
              }

              AVFrameUniquePtr captured{nullptr, avframe_and_data_deleter};
              // Pre-target frames we decode on the way to target_frame_pts.
              // After landing we seed these into the ring's history so subsequent
              // backward presses hit the L0 pivot path rather than re-firing L1
              // and re-decoding the same GOP.
              //
              // Capped at frame_buffer_size_ entries (FrameRing's history
              // capacity); excess entries at the front are dropped as we walk
              // forward so only the most recent pre-target frames are kept.
              std::deque<AVFrameUniquePtr> pre_target_frames;
              const std::size_t history_cap = frame_buffer_size_;

              // Pump one decoded frame path: hw-transfer, filter, convert,
              // check vs target_pts. Captures and stops when landed.
              auto try_land = [&](AVFrame* decoded_raw) -> bool {
                AVFrameSharedPtr decoded_sw;
                if (decoded_raw->format == dec.hw_pixel_format()) {
                  decoded_sw = AVFrameSharedPtr{av_frame_alloc(), avframe_deleter};
                  if (av_hwframe_transfer_data(decoded_sw.get(), decoded_raw, 0) < 0) return false;
                  if (av_frame_copy_props(decoded_sw.get(), decoded_raw) < 0) return false;
                } else {
                  // Take a ref-owning shared_ptr wrapping the raw frame (no copy).
                  decoded_sw = AVFrameSharedPtr{av_frame_clone(decoded_raw), avframe_deleter};
                  if (!decoded_sw) return false;
                }

                if (!flt.send(decoded_sw.get())) return false;
                const bool gpu_on = (display_ && display_->get_gpu_renderer_active());
                while (true) {
                  AVFrameUniquePtr filtered{av_frame_alloc(), avframe_deleter};
                  if (!flt.receive(filtered.get())) break;

                  AVFrameUniquePtr out;
                  if (gpu_on) {
                    // Mirror the GPU branch of format_convert_video: if filtered
                    // dims match destination, pass the filtered frame through
                    // with frame_key + original_w/h metadata tagged; otherwise
                    // rescale via the format converter.
                    const bool dims_match = (static_cast<size_t>(filtered->width) == cvt.dest_width() && static_cast<size_t>(filtered->height) == cvt.dest_height());
                    if (dims_match) {
                      const AVDictionaryEntry* gen = av_dict_get(filtered->metadata, "filter_generation", nullptr, 0);
                      const std::string frame_key = std::to_string(filtered->pts) + ":" + (gen ? gen->value : "0");
                      set_frame_key(filtered.get(), frame_key);
                      av_dict_set(&filtered->metadata, "original_width", std::to_string(filtered->width).c_str(), 0);
                      av_dict_set(&filtered->metadata, "original_height", std::to_string(filtered->height).c_str(), 0);
                      out = AVFrameUniquePtr{filtered.release(), avframe_deleter};
                    } else {
                      AVFrameUniquePtr rescaled{av_frame_alloc(), avframe_and_data_deleter};
                      if (av_frame_copy_props(rescaled.get(), filtered.get()) < 0) return false;
                      if (av_image_alloc(rescaled->data, rescaled->linesize, cvt.dest_width(), cvt.dest_height(), cvt.dest_pixel_format(), 64) < 0) return false;
                      cvt(filtered.get(), rescaled.get());
                      out = std::move(rescaled);
                    }
                  } else {
                    AVFrameUniquePtr converted{av_frame_alloc(), avframe_and_data_deleter};
                    if (av_frame_copy_props(converted.get(), filtered.get()) < 0) return false;
                    if (av_image_alloc(converted->data, converted->linesize, cvt.dest_width(), cvt.dest_height(), cvt.dest_pixel_format(), 64) < 0) return false;
                    cvt(filtered.get(), converted.get());
                    out = std::move(converted);
                  }

                  // VideoFilterer::receive rewrites pts to AV_TIME_BASE μs
                  // since demuxer start_time; compare against target_frame_pts
                  // which is in the same unit.
                  if (out->pts >= target_frame_pts) {
                    captured = std::move(out);
                    return true;
                  }
                  // Pre-target: stash for replay into ring.history (capped).
                  if (history_cap > 0) {
                    pre_target_frames.push_back(std::move(out));
                    while (pre_target_frames.size() > history_cap) pre_target_frames.pop_front();
                  }
                }
                return false;
              };

              // Feed packets from keyframe forward; stop on landing.
              bool landed = false;
              hit->range->iterate_from(hit->absolute_buffer_index, [&](const AVPacket* src_pkt) -> bool {
                if (landed) return false;
                AVPacket* cloned = av_packet_clone(src_pkt);
                if (!cloned) return false;
                dec.send(cloned);  // EAGAIN tolerated — we'll receive more on the next send
                av_packet_free(&cloned);

                while (true) {
                  AVFrame* raw = av_frame_alloc();
                  if (!raw) return false;
                  if (!dec.receive(raw, demuxers_[side].get())) {
                    av_frame_free(&raw);
                    break;
                  }
                  const bool got = try_land(raw);
                  av_frame_free(&raw);
                  if (got) {
                    landed = true;
                    return false;
                  }
                }
                return !landed;
              });

              // Decoder may still have buffered frames (B-frame reorder delay).
              // Enter draining mode with a NULL packet and keep receiving.
              if (!landed) {
                dec.send(nullptr);
                while (!landed) {
                  AVFrame* raw = av_frame_alloc();
                  if (!raw) break;
                  if (!dec.receive(raw, demuxers_[side].get())) {
                    av_frame_free(&raw);
                    break;
                  }
                  const bool got = try_land(raw);
                  av_frame_free(&raw);
                  if (got) landed = true;
                }
              }

              if (!captured) {
                l1_ok = false;
                l1_fail_reason = "no-landed-frame";
                break;
              }
              if (log_l1_stages) std::cerr << "[l1-stage] side=" << side.to_string() << " landed pts=" << captured->pts << std::endl;

              // Seed ring: replay pre-target frames into history, then land
              // current on the target. Each push_prefetch + advance() moves a
              // frame into current and kicks the previous current into history
              // (see FrameRing::advance). After the loop, history holds the
              // pre-target frames in playback order and current is the target.
              side_state.ring.clear();
              while (!pre_target_frames.empty()) {
                if (!side_state.ring.push_prefetch(std::move(pre_target_frames.front()))) break;
                pre_target_frames.pop_front();
                side_state.ring.advance();
              }
              // LEFT has no time shift. RIGHT's shift composes static + dynamic
              // (mirrors pop_and_reset's right-only shift handling in L2).
              if (side.is_left()) {
                side_state.effective_time_shift_ = 0;
                side_state.pts_ = captured->pts;
              } else {
                side_state.effective_time_shift_ = time_shifter_.static_shift() + time_shifter_.dynamic_shift(captured->pts);
                side_state.pts_ = captured->pts - side_state.effective_time_shift_;
              }
              side_state.previous_decoded_picture_number_ = -1;
              side_state.decoded_picture_number_ = 1;
              side_state.ring.set_current(std::move(captured));
              if (log_l1_stages) std::cerr << "[l1-stage] side=" << side.to_string() << " seeded history=" << side_state.ring.history_size() << std::endl;
            }
            } catch (const std::exception& ex) {
              l1_ok = false;
              l1_fail_reason = "ffmpeg-exception";
              std::cerr << "[l1-exception] " << ex.what() << std::endl;
            } catch (...) {
              l1_ok = false;
              l1_fail_reason = "unknown-exception";
              std::cerr << "[l1-exception] unknown" << std::endl;
            }
            if (log_l1_stages) std::cerr << "[l1-stage] exit ok=" << l1_ok << std::endl;

            if (!l1_ok) {
              // Decode failure mid-L1 (decoder error, EOF mid-GOP, OOM on frame
              // alloc, HW-decoder state trouble). Unwind the seek-in-progress
              // and fall through to the L2 branch: clear flags, restart queues
              // so the L2 barrier-wait succeeds.
              if (log_seek_timing) {
                std::cerr << "[l1-fail] reason=" << (l1_fail_reason ? l1_fail_reason : "unknown") << " — falling back to L2" << std::endl;
              }

              // Flush decoder state and let L2 re-run the barrier cleanly.
              for (auto& p : video_decoders_)
                if (should_seek(p.first)) {
                  p.second->flush();
                  p.second->reset_pts_state();
                }
              for (auto& p : packet_queues_)
                if (should_seek(p.first)) p.second->restart();
              for (auto& p : decoded_frame_queues_)
                if (should_seek(p.first)) p.second->restart();
              for (auto& p : filtered_frame_queues_)
                if (should_seek(p.first)) p.second->restart();
              for (auto& p : converted_frame_queues_)
                if (should_seek(p.first)) p.second->restart();
              for (auto& p : seeking_per_side_) p.second.store(false, std::memory_order_relaxed);

              seek_tier = "L1->L2";   // tag the timing log for visibility
              l1_eligible = false;     // force L2 path below
            } else {
              // Post-L1 cleanup: decoders/filterers now carry mid-stream state
              // from the inline run. Flush + reinit so the pipeline resumes on a
              // clean slate, then nudge each demuxer to just past target_pts so
              // packet_queues_ refill from there rather than replaying the GOP
              // we just decoded.
              for (auto& p : video_decoders_) {
                if (!should_seek(p.first)) continue;
                p.second->flush();
                p.second->reset_pts_state();
              }
              for (auto& p : video_filterers_) {
                if (!should_seek(p.first)) continue;
                p.second->reinit();
              }

              // Reposition each demuxer to the keyframe AT OR BEFORE target so
              // the pipeline resumes from a decoder-safe point. The worker's
              // packet stream from that keyframe includes packets whose frames
              // would re-cover the history we just seeded — intake_prefetch
              // drops any frame with pts ≤ current.pts (see the drop filter
              // there), so the ring's prefetch fills with frames strictly
              // AFTER target and the first `+` after an L1 seek advances
              // correctly instead of landing on a far-forward keyframe that
              // a post-target seek would otherwise have skipped to.
              for (auto& p : demuxers_) {
                const Side& side = p.first;
                if (!should_seek(side)) continue;
                const float target_sec = l1_target_sec[side];
                p.second->seek(target_sec, true);
              }

              for (auto& p : packet_queues_)
                if (should_seek(p.first)) p.second->restart();
              for (auto& p : decoded_frame_queues_)
                if (should_seek(p.first)) p.second->restart();
              for (auto& p : filtered_frame_queues_)
                if (should_seek(p.first)) p.second->restart();
              for (auto& p : converted_frame_queues_)
                if (should_seek(p.first)) p.second->restart();
              for (auto& p : seeking_per_side_) p.second.store(false, std::memory_order_relaxed);

              skip_update = true;
            }
          }

          if (l1_eligible) {
            // L1 handled it — skip the L2 body.
            goto after_seek_block;
          }

          // Barrier (shared with L1 re-decode and loop-mode materialize). During a
          // pure right frame shift, should_seek returns false for LEFT, keeping the
          // left pipeline alive.
          enter_seek_barrier(should_seek);

          // consume filter changes on seeking side. Others keep their pending changes
          // so they get applied on the next full seek.
          for (const auto& pair : video_filterers_) {
            if (should_seek(pair.first)) {
              pair.second->consume_filter_change();
            }
          }

          // reinit filter graphs on seeking side (reinit'ing a live filterer would
          // race with its filter_video thread).
          for (auto& pair : video_filterers_) {
            if (should_seek(pair.first)) {
              pair.second->reinit();
            }
          }

          // recalculate max dimensions
          const auto dims = calculate_max_dest_dimensions(video_filterers_);
          const bool dims_changed = (dims.first != max_width_) || (dims.second != max_height_);
          max_width_ = dims.first;
          max_height_ = dims.second;

          // if dimensions changed, recreate format converters and reinitialize video dimensions
          if (dims_changed) {
            recreate_format_converters(format_conversion_sws_flags);
            display_->reinitialize_video_dimensions(static_cast<unsigned>(max_width_), static_cast<unsigned>(max_height_));
          }

          float next_left_position;

          // the left video is the "master"
          const float min_left_position = (left.first_pts_ > INT64_MIN) ? (left.first_pts_ * AV_TIME_TO_SEC + left.start_time_) : left.start_time_;
          const float left_position = left.pts_ * AV_TIME_TO_SEC + left.start_time_;
          const bool left_is_single_frame = media_frame_detection_states_.at(LEFT).cardinality.load(std::memory_order_relaxed) == MediaFrameCardinality::SingleFrame;

          if (seek_from_start && !left_is_single_frame) {
            // seek from start based on the shortest stream duration in seconds
            next_left_position = shortest_duration_ * seek_relative + left.start_time_;
          } else {
            if (left_is_single_frame) {
              // force state transition for single frame media files
              seek_relative = left.delta_pts_ * AV_TIME_TO_SEC;
            }

            next_left_position = left_position + seek_relative;
          }

          // Clamp seeks so we never go before the first decoded PTS.
          next_left_position = std::max(next_left_position, min_left_position);

          // determine if the seek is backward or forward
          const auto all_media_are_multi_frame = [&]() -> bool {
            return std::all_of(media_frame_detection_states_.cbegin(), media_frame_detection_states_.cend(), [](const auto& kv) { return kv.second.cardinality.load(std::memory_order_relaxed) == MediaFrameCardinality::MultiFrame; });
          };
          // Absolute scrubs (timeline click, timestamp jump) must land on the keyframe
          // at-or-before the target and decode forward; without BACKWARD, av_seek_frame
          // requires a keyframe at-or-after the target, which fails on inputs with
          // sparse keyframes (e.g., a single keyframe at PTS 0).
          const bool backward = seek_from_start || (seek_relative < 0.0F) || (shift_right_frames != 0) || (force_seek_current_position && all_media_are_multi_frame());

          auto compute_right_position = [&](const SideState& right_state) -> float { return left.pts_ * AV_TIME_TO_SEC + right_state.start_time_; };

          // Seek all right videos and track failures
          bool seek_failed = false;

          // Collected per-side seek targets so the post-seek frame-drain loop in
          // pop_and_reset can advance past the landed-on keyframe to the exact
          // requested frame.
          std::map<Side, float> right_seek_positions;

          for (auto& pair : side_states) {
            const Side& side = pair.first;

            // Skip non-right sides, and under swap-aware shift-click also skip
            // rights that aren't the follower (in that case should_seek only
            // passes LEFT through, which is handled by its own block below).
            if (side.is_right() && should_seek(side)) {
              SideState& right_state = pair.second;

              float next_right_position;

              const float min_right_position = (right_state.first_pts_ > INT64_MIN) ? (right_state.first_pts_ * AV_TIME_TO_SEC + right_state.start_time_) : right_state.start_time_;
              const float right_position = compute_right_position(right_state);
              const bool right_is_single_frame = media_frame_detection_states_.at(side).cardinality.load(std::memory_order_relaxed) == MediaFrameCardinality::SingleFrame;

              if (seek_from_start && !right_is_single_frame) {
                // seek from start based on the shortest stream duration in seconds
                next_right_position = shortest_duration_ * seek_relative + right_state.start_time_;
              } else {
                if (right_is_single_frame) {
                  // force state transition for single frame media files
                  seek_relative = right_state.delta_pts_ * AV_TIME_TO_SEC;
                }

                next_right_position = right_position + seek_relative;
              }

              // Clamp seeks so we never go before the first decoded PTS.
              next_right_position = std::max(next_right_position, min_right_position);

              // Add the static and dynamic time shifts to the next right position.
              // Exception: shift-click (right_only_seek with seek_from_start) should
              // land right at the clicked timeline position in its own absolute
              // axis, ignoring the existing left↔right offset. The TimeShifter
              // update below makes sync treat left.pts_ as the new reference so
              // playback doesn't drag left forward to chase right's new position.
              const bool skip_shift_for_shift_click = right_only_seek && seek_from_start;
              if (!skip_shift_for_shift_click) {
                next_right_position += time_shifter_.static_shift() * AV_TIME_TO_SEC;
                next_right_position += static_cast<float>(time_shifter_.dynamic_shift(static_cast<int64_t>((next_right_position - right_state.start_time_) / AV_TIME_TO_SEC), false)) * AV_TIME_TO_SEC;
              }

              right_seek_positions[side] = next_right_position;

#ifdef _DEBUG
              std::cout << "SEEK:"
                        << " next_right_position=" << (int)(next_right_position * 1000)
                        << " (side=" << side.to_string() << ")"
                        << ", backward=" << backward
                        << std::endl;
#endif
              const bool right_seek_result = demuxers_[side]->seek(next_right_position, backward);
              if (!right_seek_result && !backward) {
                seek_failed = true;
              }
#ifdef _DEBUG
              std::cout << "Right seek result: " << right_seek_result << " - side: " << side.to_string() << std::endl;
#endif
            }
          }

          if (should_seek(LEFT)) {
#ifdef _DEBUG
            std::cout << "SEEK: next_left_position=" << (int)(next_left_position * 1000) << ", backward=" << backward << std::endl;
#endif
            const bool left_seek_result = demuxers_[LEFT]->seek(next_left_position, backward);
            if (!left_seek_result && !backward) {
              seek_failed = true;
            }
#ifdef _DEBUG
            std::cout << "Left seek result: " << left_seek_result << " - side: " << LEFT.to_string() << std::endl;
#endif
          }

          // Restore all positions if any seek failed on seeking side
          if (seek_failed) {
            display_->set_pending_message("Unable to seek past end of file");

            if (should_seek(LEFT)) {
              demuxers_[LEFT]->seek(left_position, true);
            }

            for (auto& pair : side_states) {
              const Side& side = pair.first;
              if (side.is_right() && should_seek(side)) {
                SideState& right_state = pair.second;
                demuxers_[side]->seek(compute_right_position(right_state), true);
              }
            }
          }

          // Clear per-side seeking flags
          for (auto& pair : seeking_per_side_) {
            pair.second.store(false, std::memory_order_relaxed);
          }

          // allow packet and frame queues to receive data again on sides we stopped
          for (auto& pair : packet_queues_) {
            if (should_seek(pair.first)) {
              pair.second->restart();
            }
          }
          for (auto& pair : decoded_frame_queues_) {
            if (should_seek(pair.first)) {
              pair.second->restart();
            }
          }
          for (auto& pair : filtered_frame_queues_) {
            if (should_seek(pair.first)) {
              pair.second->restart();
            }
          }
          for (auto& pair : converted_frame_queues_) {
            if (should_seek(pair.first)) {
              pair.second->restart();
            }
          }

          // Pop the first post-seek frame off the converted queue and seed the ring's
          // current slot with it. The caller clears the ring beforehand so that the
          // fresh current has no stale history or prefetch alongside it.
          //
          // av_seek_frame(..., AVSEEK_FLAG_BACKWARD) lands the demuxer on the
          // keyframe at-or-before the requested time. During normal playback the
          // main loop's advance_ring() naturally steps past that keyframe to the
          // frame that matches the clicked time, but while paused nothing else
          // advances — we would be stuck on the keyframe. When `drain_to_target`
          // is true (timeline clicks, arrow seeks, timestamp paste) we drain
          // intermediate frames here so paused scrubbing lands on the exact
          // clicked frame. `target_position_sec` is the seek target in the
          // side's own time axis (start_time_ plus any time shift for the right),
          // so it can be compared directly against the side's raw (pre-shift) PTS.
          //
          // Pure `+`/`-` frame shifts that spill past the ring-pivot fast path
          // skip the drain: each popped frame blocks the main loop on a pipeline
          // decode, and users press the shift keys rapidly. The slight landing
          // imprecision matches the pre-drain behavior they were already
          // tolerating for that specific case.
          auto pop_and_reset = [&](SideState& side_state, const float target_position_sec, const bool drain_to_target, int64_t* effective_time_shift = nullptr) {
            AVFrameUniquePtr first_frame{nullptr, avframe_deleter};
            if (!converted_frame_queues_[side_state.side_]->pop(first_frame) || first_frame == nullptr) {
#ifdef _DEBUG
              std::cout << "Side state frame is nullptr: " << side_state.side_.to_string() << std::endl;
#endif
              return;
            }

            if (drain_to_target) {
              const int64_t target_pts = static_cast<int64_t>(std::llround((static_cast<double>(target_position_sec) - side_state.start_time_) / AV_TIME_TO_SEC));

              // Keep the most recently popped frame whose PTS is < target_pts, so
              // that if we run out of frames (EOF / stopped queue) we don't throw
              // away progress.
              while (first_frame->pts < target_pts) {
                AVFrameUniquePtr next_frame{nullptr, avframe_deleter};
                if (!converted_frame_queues_[side_state.side_]->pop(next_frame) || next_frame == nullptr) {
                  break;
                }
                first_frame = std::move(next_frame);
              }
            }

            side_state.pts_ = first_frame->pts;

            // if the effective time shift is provided, update it and subtract it from the PTS
            if (effective_time_shift != nullptr) {
              *effective_time_shift += time_shifter_.dynamic_shift(first_frame->pts);
              side_state.pts_ -= *effective_time_shift;
            }

            side_state.previous_decoded_picture_number_ = -1;
            side_state.decoded_picture_number_ = 1;
            side_state.ring.set_current(std::move(first_frame));
          };

          // Drain only when paused and not doing a pure `+`/`-` frame shift.
          // Rationale:
          //   - Playing seeks: the main loop's timer-based catch-up already
          //     advances rapidly from the keyframe to the target (us_until_target
          //     goes negative, skip_update stays false). Blocking the main
          //     thread here would freeze the UI for a full GOP duration and
          //     leaves stale overlay values (e.g. quality metrics) visible.
          //   - Pure `+`/`-` fall-throughs: each popped frame blocks on a
          //     pipeline decode and users press these keys rapidly, so tolerate
          //     the landing imprecision for responsiveness.
          //   - Paused user-initiated scrubs (timeline click, arrow keys,
          //     timestamp paste): drain so the single paused frame shown after
          //     the click is exactly the clicked frame.
          //   - Auto-align retries: though the shift goes through
          //     shift_right_frames (so pure_right_frame_shift is true), the
          //     user expects a precise landing — there's no key-mashing
          //     responsiveness concern. Force-drain so the landed frame is
          //     exactly the best_pts the algorithm chose.
          const bool drain_to_target =
              (!pure_right_frame_shift || pending_auto_align_verification_.active) && !display_->get_play();

          if (should_seek(LEFT)) {
            left.ring.clear();
            pop_and_reset(left, next_left_position, drain_to_target);
          }

          // Shift-click (right-only seek from start): one side didn't move,
          // the other jumped to an absolute timeline position in its own
          // axis. Update TimeShifter so the post-seek pts_ values align —
          // otherwise sync_frame_queue would drag the stationary side
          // forward to chase the moved side.
          //
          // TimeShifter's static_shift continues to mean "right-relative-to-
          // left" regardless of which side moved. Under swap the follower
          // is LEFT; the sign of the update flips accordingly so the
          // invariant holds.
          if (right_only_seek && seek_from_start && right_delta > 0) {
            // TimeShifter tracks "right raw PTS − left raw PTS". After a
            // right-only seek we want the stationary side's raw PTS to line
            // up with the follower's new position in common time. Derivation:
            //   new_static_shift = right_raw − left_raw
            //   For follower=RIGHT: right_raw = target (just seeked there),
            //                       left_raw = left.pts_ (stationary).
            //   For follower=LEFT : left_raw = target (just seeked there),
            //                       right_raw = right_ptr->pts_ + old_shift
            //                                   (stationary; recover raw from
            //                                    stored common time + old
            //                                    static_shift).
            const int64_t old_static_shift_us = time_shifter_.static_shift();
            int64_t new_static_shift_us = 0;
            if (follower_side.is_right()) {
              const float nrp = right_seek_positions[right_ptr->side_];
              const int64_t expected_right_raw_us = static_cast<int64_t>(static_cast<double>(nrp - right_ptr->start_time_) * AV_TIME_BASE);
              new_static_shift_us = expected_right_raw_us - left.pts_;
            } else {
              const int64_t right_raw_us = right_ptr->pts_ + old_static_shift_us;
              const int64_t expected_left_raw_us = static_cast<int64_t>(static_cast<double>(next_left_position - left.start_time_) * AV_TIME_BASE);
              new_static_shift_us = right_raw_us - expected_left_raw_us;
            }
            total_right_time_shifted = static_cast<int>((new_static_shift_us - time_shifter_.offset_av_time()) / right_delta);
            time_shifter_.set_frame_shift_accumulator(total_right_time_shifted, right_delta);

            // The stationary side doesn't go through pop_and_reset, so its
            // cached `pts_` and `effective_time_shift_` are about to be
            // stale. Recompute them from the new static_shift; otherwise
            // sync_frame_queue will see a phantom PTS divergence and drag
            // the stationary side forward trying to "catch up".
            if (follower_side.is_left()) {
              // Right is stationary. Its raw PTS didn't change.
              const int64_t right_raw_us = right_ptr->pts_ + old_static_shift_us;
              right_ptr->effective_time_shift_ = time_shifter_.effective_shift(right_raw_us);
              right_ptr->pts_ = right_raw_us - right_ptr->effective_time_shift_;
            }
            // (follower_side == RIGHT case: left is stationary and master;
            // left.pts_ is already on the common axis so nothing to update.)
          }

          // Nudge near-zero residual shifts away from zero so the right demuxer lands
          // on a distinct frame. Exact integer-frame shifts pass through unchanged.
          time_shifter_.nudge_away_from_zero(right_ptr->delta_pts_);

          // Reset each right video that participated in this seek. Under the
          // swap-aware shift-click / auto-align paths the follower may be
          // LEFT, in which case RIGHT didn't seek at all and its ring must
          // not be cleared; `should_seek(side)` gates it correctly.
          for (auto& pair : side_states) {
            const Side& side = pair.first;
            if (side.is_right() && should_seek(side)) {
              SideState& right_state = pair.second;

              right_state.ring.clear();
              right_state.effective_time_shift_ = time_shifter_.static_shift();
              pop_and_reset(right_state, right_seek_positions[side], drain_to_target, &right_state.effective_time_shift_);
            }
          }

          // don't sync until the next iteration to prevent freezing when comparing a single image
          skip_update = true;
        }  // end of full-seek branch
      after_seek_block:;
        log_seek();

        // --- Post-seek auto-align verification ---
        // If the seek we just completed was driven by the auto-align block
        // earlier this iteration, rescore the pair against the same left-probe
        // fingerprints using the right ring's newly-landed current frame. We
        // only report — no retry, no rollback — matching the user's chosen
        // "warn only, leave as-is" policy for seek-imprecision surfacing.
        if (pending_auto_align_verification_.active) {
          auto& v = pending_auto_align_verification_;
          const bool log_auto_align = env_flag_enabled("VIDEO_COMPARE_LOG_AUTO_ALIGN");

          // Fresh SwsContext lookup + fingerprint helpers — duplicated from the
          // auto-align block so this code path is self-contained. Low cost: at
          // most a handful of ring frames need a 64x64 sws pass (~30 µs each).
          const auto get_or_build_sws = [this](AVPixelFormat fmt, int w, int h) -> SwsContext* {
            const AutoAlignSwsKey key{fmt, w, h};
            const auto it = auto_align_sws_cache_.find(key);
            if (it != auto_align_sws_cache_.end()) {
              return it->second.get();
            }
            SwsContext* raw = sws_getContext(w, h, fmt, MetricsCalculator::kFingerprintSize, MetricsCalculator::kFingerprintSize, AV_PIX_FMT_GRAY8, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (raw == nullptr) {
              return nullptr;
            }
            const auto ins = auto_align_sws_cache_.emplace(key, SwsContextUniquePtr(raw));
            return ins.first->second.get();
          };
          const auto fingerprint_ring_frame = [&](const AVFrame* frame, std::vector<float>& out) -> bool {
            if (frame == nullptr || frame->width <= 0 || frame->height <= 0) {
              return false;
            }
            SwsContext* const ctx = get_or_build_sws(static_cast<AVPixelFormat>(frame->format), frame->width, frame->height);
            if (ctx == nullptr) {
              return false;
            }
            MetricsCalculator::compute_structural_fingerprint(frame->data, frame->linesize, frame->height, ctx, out);
            return !out.empty();
          };
          const auto find_nearest_in_ring = [](const FrameRing& ring, int64_t target_pts, int64_t tolerance_pts) -> const AVFrame* {
            const int min_off = -ring.history_size();
            const int max_off = ring.prefetch_size();
            const AVFrame* best = nullptr;
            int64_t best_dist = std::numeric_limits<int64_t>::max();
            for (int off = min_off; off <= max_off; ++off) {
              const AVFrame* const f = ring.at(off);
              if (f == nullptr) {
                continue;
              }
              const int64_t dist = std::abs(f->pts - target_pts);
              if (dist < best_dist) {
                best_dist = dist;
                best = f;
              }
            }
            if (best == nullptr || best_dist > tolerance_pts) {
              return nullptr;
            }
            return best;
          };

          // The follower side is the one we actually seeked; its FrameRing
          // holds the landed frame we need to rescore against.
          const SideState& follower_after = (v.follower_side.is_left()) ? left : *right_ptr;
          const FrameRing& follower_ring_after = follower_after.ring;
          const AVFrame* const follower_current_after = follower_ring_after.current_frame();
          const int64_t follower_tolerance = v.probe_step_pts / 2;

          // Rescore the same 5-probe window against the post-seek follower
          // ring. For each stored master-probe fingerprint, find the follower
          // frame nearest to (master_probe_pts + delta_t_pts), fingerprint it
          // fresh, correlate.
          float sum = 0.0f;
          int n_valid = 0;
          for (const auto& pair : v.master_probe_fingerprints) {
            const int64_t m_pts = pair.first;
            const std::vector<float>& m_fp = pair.second;
            const int64_t f_target = m_pts + v.delta_t_pts;
            const AVFrame* const f_frame = find_nearest_in_ring(follower_ring_after, f_target, follower_tolerance);
            if (f_frame == nullptr) {
              continue;
            }
            std::vector<float> f_fp;
            if (!fingerprint_ring_frame(f_frame, f_fp)) {
              continue;
            }
            sum += MetricsCalculator::structural_correlation(m_fp, f_fp);
            ++n_valid;
          }

          const float landed_score = (n_valid >= 3) ? (sum / static_cast<float>(n_valid)) : -std::numeric_limits<float>::max();
          constexpr float kAutoAlignVerificationSlack = 0.01f;
          const bool ok = (n_valid >= 3) && (landed_score >= v.expected_score - kAutoAlignVerificationSlack);
          const int64_t landed_pts_delta = (follower_current_after != nullptr) ? (follower_current_after->pts - v.master_current_pts) : 0;

          if (!ok && n_valid >= 3) {
            display_->set_pending_message(string_sprintf("Auto-align: landed score %.3f (expected %.3f) — seek imprecision", landed_score, v.expected_score));
          }
          // When the follower is RIGHT (standard, non-swap case), override
          // TimeShifter::static_shift to the EXACT raw right-vs-left delta.
          // Without this override, static_shift is `total_frames × avg_delta`
          // and drifts by a few ms under variable frame durations (e.g.,
          // 59.98 fps VP9 with 16/17 ms alternating deltas truncated to an
          // integer average). We use the ACTUAL landed pts regardless of
          // verification probe count — verification may not find enough
          // probe matches in a small-capacity ring post-pivot, but the
          // landed pts is ground truth either way.
          if (v.follower_side.is_right() && follower_current_after != nullptr) {
            const AVFrame* const left_current = left.ring.current_frame();
            if (left_current != nullptr) {
              time_shifter_.set_static_shift_us(follower_current_after->pts - left_current->pts);
              right_ptr->effective_time_shift_ = time_shifter_.effective_shift(follower_current_after->pts);
              right_ptr->pts_ = follower_current_after->pts - right_ptr->effective_time_shift_;
            }
          }

          // Sync the retry cache's follower_pts_at_cache to the actual landed
          // PTS so the next auto-align press (same or different key) can
          // reuse the cached candidate pool and master probes, and extend
          // from the current searched interval. Without this, the next
          // press's invalidation check would see a mismatch between
          // retry.follower_pts_at_cache (our pre-seek estimate) and the
          // actual landed follower_current and throw away the cache.
          if (auto_align_retry_cache_.valid && follower_current_after != nullptr) {
            auto_align_retry_cache_.follower_pts_at_cache = follower_current_after->pts;
          }
          // When ok, the main-pass "shift %+d (score %.3f)" message set earlier
          // stays on screen — no need to overwrite it with a near-identical one.

          if (log_auto_align) {
            std::cerr << "[auto-align]"
                      << " landed_pts_delta_ms=" << string_sprintf("%.3f", static_cast<double>(landed_pts_delta) / 1000.0)
                      << " landed_score=" << string_sprintf("%.4f", landed_score)
                      << " expected_score=" << string_sprintf("%.4f", v.expected_score)
                      << " ok=" << (ok ? "true" : "false")
                      << " probes=" << n_valid
                      << std::endl;
          }

          v = {};  // clear for next press
        }
      }

      bool store_frames = false;
      bool adjusting = false;

      // keep showing currently displayed frame for another iteration?
      const bool paused_forward_step = !display_->get_play() && forward_navigate_frames > 0;
      skip_update = skip_update || ((timer_->us_until_target() - refresh_time_deque.average()) > 0 && !paused_forward_step);
      const bool fetch_next_frame = display_->get_play() || (forward_navigate_frames > 0);

      // use the delta between current and previous PTS as the tolerance which determines whether we have to adjust
      const int64_t min_delta = compute_min_delta(left.delta_pts_, right_ptr->delta_pts_);

#ifdef _DEBUG
      const std::string current_state = string_sprintf("left_pts=%5d, left_is_behind=%d, right_pts=%5d, right_is_behind=%d, min_delta=%5d, effective_right_time_shift=%5d", left.pts_ / 1000, is_behind(left.pts_, right_ptr->pts_, min_delta),
                                                       (right_ptr->pts_ + time_shifter_.static_shift()) / 1000, is_behind(right_ptr->pts_, left.pts_, min_delta), min_delta / 1000, right_ptr->effective_time_shift_ / 1000);

      if (current_state != previous_state) {
        std::cout << current_state << std::endl;
      }

      previous_state = current_state;
#endif
      // Drain any frames that the converter produced during the seek block (they may
      // have slipped through between the earlier intake call and now, especially on
      // the skip-left-flush path where left's pipeline kept running).
      intake_prefetch();

      // Consume one frame from the ring (normal playback step or sync adjustment). If
      // prefetch is empty, fall back to a blocking pop from the converter queue to keep
      // pipeline-paced behavior when playback runs ahead of the pipeline.
      auto advance_ring = [&](SideState& side_state) {
        if (side_state.ring.prefetch_size() == 0) {
          AVFrameUniquePtr next{nullptr, avframe_deleter};
          if (!converted_frame_queues_[side_state.side_]->pop(next) || next == nullptr) {
            return false;
          }
          side_state.ring.push_prefetch(std::move(next));
        }
        if (!side_state.ring.advance()) {
          return false;
        }
        side_state.decoded_picture_number_++;
        return true;
      };

      auto sync_frame_queue = [&](SideState& side_state, const SideState& other_side) {
        if (is_behind(side_state.pts_, other_side.pts_, min_delta)) {
          adjusting = true;
          advance_ring(side_state);
        }
      };

      // sync left with all right videos
      for (auto& pair : side_states) {
        if (pair.first.is_right()) {
          SideState& right_state = pair.second;
          sync_frame_queue(left, right_state);
          sync_frame_queue(right_state, left);
        }
      }

      // handle regular playback only
      if (!skip_update && display_->get_buffer_play_loop_mode() == Display::Loop::Off) {
        if (!adjusting && fetch_next_frame) {
          // Advance the cursor on every side.
          bool all_advanced = true;
          for (auto& pair : side_states) {
            all_advanced = advance_ring(pair.second) && all_advanced;
          }

          if (!all_advanced) {
            // Some side is out of prefetched frames and its pipeline is stopped (EOF).
            // Don't mark store_frames; the ring's current remains the last successfully
            // decoded frame, so the display just keeps showing it.
            timer_->update();
          } else {
            store_frames = true;

            for (auto& pair : side_states) {
              if (pair.first.is_right()) {
                auto& side_state = pair.second;
                side_state.effective_time_shift_ = time_shifter_.effective_shift(side_state.ring.current_frame()->pts);
              }
            }

            // update timer for regular playback
            if (frame_number > 0) {
              const int64_t play_frame_delay = compute_frame_delay(left.ring.current_frame()->pts - left.pts_, right_ptr->ring.current_frame()->pts - right_ptr->pts_ - right_ptr->effective_time_shift_);

              const float pace_divisor = display_->get_play() ? display_->get_playback_speed_factor() : 1.0f;
              timer_->shift_target(static_cast<int64_t>(play_frame_delay / pace_divisor));
            } else {
              timer_->update();
            }

            // update first PTS for all videos
            for (auto& pair : side_states) {
              if (pair.second.first_pts_ == INT64_MIN && pair.second.ring.current_frame() != nullptr) {
                pair.second.first_pts_ = pair.second.ring.current_frame()->pts;
              }
            }
          }
        } else {
          timer_->reset();
        }
      }

      // for frame-accurate forward navigation, decrement counter when frame is stored in buffer
      if (store_frames && (forward_navigate_frames > 0)) {
        forward_navigate_frames--;
      }

      auto update_frame_timing = [](SideState& side_state, const int64_t& time_shift) {
        AVFrame* current = side_state.ring.current_frame();
        if (current == nullptr) {
          return;
        }
        // determine time-shifted PTS (note: new_pts only differs from current->pts on the right side)
        const int64_t new_pts = current->pts - time_shift;

        if ((side_state.decoded_picture_number_ - side_state.previous_decoded_picture_number_) == 1) {
          // compute the average PTS delta in a rolling-window fashion
          const int64_t last_duration = new_pts - side_state.pts_;
          side_state.frame_duration_deque_.push_back(last_duration);
          side_state.delta_pts_ = side_state.frame_duration_deque_.average();
        }

        if (side_state.delta_pts_ > 0) {
          // use the average PTS delta for frame duration
          ffmpeg::frame_duration(current) = side_state.delta_pts_;

          // If the oldest history entry is the FIRST decoded frame, backfill its
          // duration now that the second frame has been decoded.
          const int oldest_idx = side_state.ring.history_size();
          if (oldest_idx > 0) {
            AVFrame* oldest = side_state.ring.at(-oldest_idx);
            if (oldest != nullptr && oldest->pts == side_state.first_pts_) {
              ffmpeg::frame_duration(oldest) = side_state.delta_pts_;
            }
          }
        } else {
          side_state.delta_pts_ = ffmpeg::frame_duration(current);
        }

        side_state.pts_ = new_pts;
        side_state.previous_decoded_picture_number_ = side_state.decoded_picture_number_;
      };

      update_frame_timing(left, 0);

      for (auto& pair : side_states) {
        const Side& side = pair.first;
        if (side.is_right()) {
          SideState& right_state = pair.second;
          update_frame_timing(right_state, right_state.effective_time_shift_);
        }
      }

      bool all_stopped = true;
      for (auto& pair : converted_frame_queues_) {
        all_stopped = all_stopped && pair.second->is_stopped();
      }

      const bool no_activity = !skip_update && !adjusting && !store_frames;
      const bool end_of_file = no_activity && all_stopped;
      // Auto-loop trigger: was "FrameRing full" (≈frame_buffer_size_ frames, a
      // fraction of a second with -f 12). Now triggers when each PacketRing
      // holds at least `loop_cap_sec` of content so the first auto-loop has
      // a meaningful span. Falls back to the FrameRing-full criterion if any
      // side lacks a PacketRing.
      bool buffer_is_full = true;
      for (auto& p : side_states) {
        SideState& ss = p.second;
        auto ring_it = packet_rings_.find(ss.side_);
        if (ring_it == packet_rings_.end() || !ring_it->second) {
          buffer_is_full = ss.ring.history_plus_current_size() == static_cast<int>(frame_buffer_size_);
          if (!buffer_is_full) break;
          continue;
        }
        const auto s = ring_it->second->stats();
        if (s.pts_max == INT64_MIN || s.pts_min == INT64_MIN) {
          buffer_is_full = false;
          break;
        }
        const AVRational stream_tb = demuxers_[ss.side_]->time_base();
        const double span_sec = static_cast<double>(s.pts_max - s.pts_min) * stream_tb.num / static_cast<double>(stream_tb.den);
        if (span_sec < loop_cap_sec) {
          buffer_is_full = false;
          break;
        }
      }

      // If we're frame-stepping and hit EOF, stop trying to fetch more frames.
      if (end_of_file && (forward_navigate_frames > 0) && !display_->get_play()) {
        forward_navigate_frames = 0;
      }

      // Browse span: user's `frame_offset` walks back through (current + history). The
      // prefetch side isn't exposed here — pressing `+` on the UI is a cursor advance,
      // not a browse.
      const int last_common_frame_index = std::min(left.ring.history_plus_current_size(), right_ptr->ring.history_plus_current_size()) - 1;

      auto adjust_frame_offset = [last_common_frame_index](const int frame_offset, const int adjustment) { return std::min(std::max(0, frame_offset + adjustment), last_common_frame_index); };

      frame_offset = adjust_frame_offset(frame_offset, display_->get_frame_buffer_offset_delta());

      bool ui_refresh_performed = false;

      if (frame_offset >= 0 && last_common_frame_index >= 0) {
        const bool is_playback_in_sync = is_in_sync(left.pts_, right_ptr->pts_, left.delta_pts_, right_ptr->delta_pts_);
        display_->set_playback_in_sync(is_playback_in_sync);
        display_->set_frame_buffer_counts(left.ring.history_size(), left.ring.prefetch_size());

        // reduce refresh rate to 10 Hz for faster re-syncing
        const bool skip_refresh = !is_playback_in_sync && display_refresh_timer.us_until_target() > -RESYNC_UPDATE_RATE_US;

        if (!skip_refresh) {
          // `frame_offset` of 0 is the current frame; positive values index back into
          // history (hence the sign flip when calling `ring.at(-frame_offset)`).
          FrameRing& left_ring = !display_->get_swap_left_right() ? left.ring : right_ptr->ring;
          FrameRing& right_ring = !display_->get_swap_left_right() ? right_ptr->ring : left.ring;

          const auto left_display_frame = left_ring.at(-frame_offset);
          const auto right_display_frame = right_ring.at(-frame_offset);
          const auto left_state_frame = left.ring.at(-frame_offset);
          const auto right_state_frame = right_ptr->ring.at(-frame_offset);

          auto refresh_from_frame_if_changed = [&](SideState& side_state, const AVFrame* frame) {
            const int frame_filter_generation = VideoFilterer::get_filter_generation_from_frame(frame);

            // Use filter generation as a cheap dirty bit and skip string work for unchanged frames.
            if (frame_filter_generation < 0 || frame_filter_generation == side_state.last_filter_generation_) {
              return false;
            }

            const std::string frame_filters = VideoFilterer::get_resolved_filters_from_frame(frame);
            if (frame_filters.empty()) {
              return false;
            }

            refresh_side_filter_metadata(side_state.side_, frame_filters);

            side_state.last_filter_generation_ = frame_filter_generation;
            side_state.last_filter_description_ = frame_filters;

            return true;
          };

          bool metadata_changed = false;
          metadata_changed = refresh_from_frame_if_changed(left, left_state_frame) || metadata_changed;
          metadata_changed = refresh_from_frame_if_changed(*right_ptr, right_state_frame) || metadata_changed;

          // Rebuild metadata overlay only when at least one side's filter metadata actually changed.
          if (metadata_changed) {
            display_->update_metadata(left_video_metadata_, right_video_info_[active_right].metadata);
          }

          // count the number of unique in-sync video frame combinations processed
          if (is_playback_in_sync) {
            const std::string frame_combo_tag = get_frame_key(left_display_frame) + "|" + get_frame_key(right_display_frame);

            if (frame_combo_tag != previous_frame_combo_tag) {
              unique_frame_combo_tags_processed++;
              previous_frame_combo_tag = frame_combo_tag;
            }
          }

          // conditionally refresh display in an attempt to keep up with the target playback speed
          const uint64_t next_refresh_frame_number = lrintf(next_refresh_at);

          if (frame_number >= next_refresh_frame_number) {
            std::string prefix_str, suffix_str;

            // add [] to the current / total browsable string when in sync
            if (fetch_next_frame && is_playback_in_sync) {
              prefix_str = "[";
              suffix_str = "]";
            }

            const std::string current_total_browsable = string_sprintf(frame_offset_format_str.c_str(), prefix_str.c_str(), frame_offset + 1, last_common_frame_index + 1, suffix_str.c_str());

            // conditionally update the display; otherwise, sleep to conserve resources
            display_refresh_timer.update();

            if (display_->possibly_refresh(left_display_frame, right_display_frame, current_total_browsable)) {
              // Energy-saving gating for scope windows
              const auto scope_sample = ScopeUpdateState::capture(left_display_frame, right_display_frame, scope_window_roi, display_->get_swap_left_right());
              const bool scope_state_changed = scope_update_state_.has_changed(scope_sample);

              if (scope_state_changed) {
                scope_manager_->submit_jobs(left_display_frame, right_display_frame);
                scope_manager_->wait_all();

                scope_update_state_.update(scope_sample);

                if (scope_manager_->has_fatal_error()) {
                  throw std::runtime_error(scope_manager_->fatal_error_message());
                }
                scope_manager_->render_all();
              }

              refresh_time_deque.push_back(-display_refresh_timer.us_until_target());
            } else {
              sleep_for_ms(refresh_time_deque.average() / 1000);
            }

            ui_refresh_performed = true;

            // calculate next refresh time dynamically based on target playback speed and current refresh timing
            const double target_time_us = std::max(1000.0, static_cast<double>(std::max(ffmpeg::frame_duration(left_display_frame), ffmpeg::frame_duration(right_display_frame))) / display_->get_playback_speed_factor());
            const double refresh_time_us = static_cast<double>(refresh_time_deque.average());

            // When the display refresh is slower than the content's target frame rate,
            // display every produced frame rather than skipping to maintain real-time speed.
            // Smooth slow-motion is preferable to choppy frame-skipping for a comparison tool.
            const double effective_target_us = std::max(target_time_us, refresh_time_us);
            next_refresh_at += std::max(1.0 + (frame_number - next_refresh_frame_number), refresh_time_us / effective_target_us);
          }

          // check if sleeping is the best option for accurate playback by taking the average refresh time into account
          const int64_t time_until_final_refresh = timer_->us_until_target();

          if (!adjusting && time_until_final_refresh > 0 && time_until_final_refresh < refresh_time_deque.average()) {
            timer_->wait(time_until_final_refresh);
          } else if (time_until_final_refresh <= 0 && display_->get_buffer_play_loop_mode() != Display::Loop::Off) {
            // auto-adjust current frame during in-buffer playback
            switch (display_->get_buffer_play_loop_mode()) {
              case Display::Loop::ForwardOnly:
                if (frame_offset == 0) {
                  frame_offset = last_common_frame_index;
                } else {
                  frame_offset = adjust_frame_offset(frame_offset, -1);
                }
                break;
              case Display::Loop::PingPong:
                if (last_common_frame_index >= 1 && (frame_offset == 0 || frame_offset == last_common_frame_index)) {
                  display_->toggle_buffer_play_direction();
                }
                frame_offset = adjust_frame_offset(frame_offset, display_->get_buffer_play_forward() ? -1 : 1);
                break;
              default:
                break;
            }

            // update timer for accurate in-buffer playback
            const int64_t in_buffer_frame_delay = compute_frame_delay(ffmpeg::frame_duration(left.ring.at(-frame_offset)), ffmpeg::frame_duration(right_ptr->ring.at(-frame_offset)));

            timer_->shift_target(in_buffer_frame_delay / display_->get_playback_speed_factor());
          }

          // enter in-buffer playback once if buffer is full or EOF reached
          if (auto_loop_mode_ != Display::Loop::Off && !auto_loop_triggered && (buffer_is_full || end_of_file)) {
            display_->set_buffer_play_loop_mode(auto_loop_mode_);

            auto_loop_triggered = true;
          }
        }
      }

      if (ui_refresh_performed) {
        full_cycle_time_deque.push_back(-full_cycle_timer.us_until_target());

        // update video/UI frame rate string every second (or if deque gets full)
        if ((full_cycle_time_deque.sum() > NOMINAL_FPS_UPDATE_RATE_US) || full_cycle_time_deque.full()) {
          auto calculate_fps = [](const uint32_t num, const uint32_t denom) { return static_cast<float>(num) / static_cast<float>(denom); };

          const float video_fps = calculate_fps(ONE_SECOND_US * unique_frame_combo_tags_processed, full_cycle_time_deque.sum());
          const float ui_fps = calculate_fps(ONE_SECOND_US, full_cycle_time_deque.average());

          fps_message = string_sprintf("Video/UI FPS: %.1f/%.1f", video_fps, ui_fps);
          display_->set_current_fps(video_fps, ui_fps);

          full_cycle_time_deque.clear();
          unique_frame_combo_tags_processed = 0;
        }
      }

      // Publish the latest playback-position snapshot for external inspectors
      // (debug input socket, future introspection tools). Runs once per main-
      // loop iteration after side_states have been advanced; contention on
      // the mutex is negligible because reads are rare.
      {
        const SideState& right_now = *right_ptr;
        std::lock_guard<std::mutex> lock(playback_state_snapshot_mutex_);
        playback_state_snapshot_.left_pts_us = left.pts_;
        playback_state_snapshot_.right_pts_us = right_now.pts_;
        playback_state_snapshot_.effective_time_shift_us = right_now.effective_time_shift_;
        playback_state_snapshot_.left_decoded_picture_number = left.previous_decoded_picture_number_;
        playback_state_snapshot_.right_decoded_picture_number = right_now.previous_decoded_picture_number_;
        playback_state_snapshot_.frame_number = frame_number;
        playback_state_snapshot_.initialized = true;
      }
    }
  } catch (...) {
    exception_holder_.store_current_exception();
  }

  // Quit queues for all videos (left and all right videos)
  quit_all_queues();
}

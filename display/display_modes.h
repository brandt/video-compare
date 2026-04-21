#pragma once

enum class DisplayMode { Split, VStack, HStack };
enum class DisplayLoop { Off, ForwardOnly, PingPong };
enum class DisplayAspectLockMode { Off, Window, Content };
enum class DisplayAspectViewMode { Stretch, Original, Preset16x9, Preset4x3, Preset1x1 };

/**
 * High-level playback state label, derived from the combination of play/pause,
 * loop mode, and PTS-sync status. Order encodes rendering priority: Seek
 * outranks loop modes, loop modes outrank Play, Play outranks Pause.
 */
enum class DisplayPlayState { Seek, LoopForward, LoopPingPong, Play, Pause };

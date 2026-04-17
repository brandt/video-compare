// Single C translation unit that provides the libplacebo libav helper
// implementations. The header is inline-only in C and needs a
// PL_LIBAV_IMPLEMENTATION guard in C++ — easiest to just compile it once
// from plain C.
#define PL_LIBAV_IMPLEMENTATION 1
#include <libplacebo/utils/libav.h>

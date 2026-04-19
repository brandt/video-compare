CXXFLAGS = -g3 -O3 -ffast-math -std=c++20 -D__STDC_CONSTANT_MACROS \
		   -I. \
		   -Wall -Wextra -Wno-deprecated -Wno-deprecated-declarations \
		   -Wdisabled-optimization -Wctor-dtor-privacy \
		   -Woverloaded-virtual -Wno-unused -Wno-missing-field-initializers

ifneq ($(filter MINGW%,$(shell uname)),)
  FFMPEG_VERSION = 8.1-full_build-shared
  SDL3_VERSION = 3.4.4
  SDL3_TTF_VERSION = 3.2.2

  FFMPEG_PATH = ffmpeg-$(FFMPEG_VERSION)
  SDL3_PATH = SDL3-devel-$(SDL3_VERSION)-mingw/SDL3-$(SDL3_VERSION)/x86_64-w64-mingw32
  SDL3_TTF_PATH = SDL3_ttf-devel-$(SDL3_TTF_VERSION)-mingw/SDL3_ttf-$(SDL3_TTF_VERSION)/x86_64-w64-mingw32

  CXX = x86_64-w64-mingw32-g++
  CXXFLAGS += -I$(FFMPEG_PATH)/include/ \
              -I$(SDL3_PATH)/include/ \
              -I$(SDL3_TTF_PATH)/include/
  LDLIBS += -L$(FFMPEG_PATH)/lib/ \
            -L$(SDL3_PATH)/lib/ \
            -L$(SDL3_TTF_PATH)/lib/
else
  CXX = g++
  LDLIBS = -pthread
endif

# Homebrew stripped down the features built into ffmpeg and created a new
# "keg-only" package that includes stuff like VMAF. "keg-only" means it
# doesn't symlink into the usual Homebrew paths. See: `brew info ffmpeg-full`
ifneq "$(wildcard /opt/homebrew/opt/ffmpeg-full)" ""
  CXXFLAGS += -I/opt/homebrew/opt/ffmpeg-full/include/
  LDLIBS += -L/opt/homebrew/opt/ffmpeg-full/lib/
  PKG_CONFIG_PATH := /opt/homebrew/opt/ffmpeg-full/lib/pkgconfig:$(PKG_CONFIG_PATH)
endif

ifneq "$(wildcard /opt/homebrew)" ""
  CXXFLAGS += -I/opt/homebrew/include/
  LDLIBS += -L/opt/homebrew/lib/
  BINDIR = /opt/homebrew/bin/
else ifneq "$(wildcard /opt/local)" ""
  CXXFLAGS += -I/opt/local/include/
  LDLIBS += -L/opt/local/lib/
  BINDIR = /opt/local/bin/
else
  CXXFLAGS += -I/usr/local/include/
  LDLIBS += -L/usr/local/lib/
  BINDIR = /usr/local/bin/
endif

ifneq "$(wildcard /usr/include/ffmpeg)" ""
  CXXFLAGS += -I/usr/include/ffmpeg
endif

# libplacebo GPU renderer (Vulkan via MoltenVK on macOS)
ifneq "$(wildcard /opt/homebrew/opt/libplacebo)" ""
  CXXFLAGS += -I/opt/homebrew/opt/libplacebo/include/
  LDLIBS += -L/opt/homebrew/opt/libplacebo/lib/
endif
LDLIBS += -lplacebo

# Default: try to use pkg-config if available and SDL3 is provided by pkg-config.
# Contributors can still force behavior with `make USE_PKG_CONFIG=0` or `=1`.
# Decide the default based on whether pkg-config advertises SDL3.
PKG_CONFIG_EXISTS := $(shell command -v pkg-config 2>/dev/null || true)
PKG_HAS_SDL3 := $(shell $(PKG_CONFIG_EXISTS) >/dev/null 2>&1 && pkg-config --exists sdl3 SDL3_ttf && echo 1 || echo 0)

ifeq ($(PKG_HAS_SDL3),1)
  USE_PKG_CONFIG ?= 1
else
  USE_PKG_CONFIG ?= 0
endif

ifeq ($(USE_PKG_CONFIG),1)
  CXXFLAGS += $(shell pkg-config --cflags libavformat libavcodec libavfilter libavutil libswscale libswresample sdl3 SDL3_ttf)
  LDLIBS   += $(shell pkg-config --libs libavformat libavcodec libavfilter libavutil libswscale libswresample sdl3 SDL3_ttf)
else
  LDLIBS += -lavformat -lavcodec -lavfilter -lavutil -lswscale -lswresample -lSDL3_ttf -lSDL3
endif

cpp_src = $(wildcard *.cpp) $(wildcard */*.cpp) $(wildcard */*/*.cpp)
c_src = $(wildcard *.c) $(wildcard */*.c)
obj = $(cpp_src:.cpp=.o) $(c_src:.c=.o)
dep = $(obj:.o=.d)
target = video-compare

all: $(target)

$(target): $(obj)
	$(CXX) -o $@ $^ $(LDLIBS)

-include $(dep)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -MF $(@:.o=.d) -c -o $@ $<

CFLAGS = -g3 -O3 -ffast-math -D__STDC_CONSTANT_MACROS -Wall -Wextra -Wno-unused
CC ?= cc

# Inherit the same include/lib paths as C++ (strip C++-only flags)
C_INCLUDES = $(filter -I%,$(CXXFLAGS))

%.o: %.c
	$(CC) $(CFLAGS) $(C_INCLUDES) -MMD -MP -MF $(@:.o=.d) -c -o $@ $<

test: $(target)
	./$(target) -w 800x screenshot_1.jpg screenshot_2.jpg

.PHONY: clean
clean:
	$(RM) $(obj) $(target) $(dep)

install: $(target)
	install -s video-compare $(BINDIR)

CXXFLAGS = -g3 -Ofast -std=c++14 -D__STDC_CONSTANT_MACROS \
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

# Default: don't use pkg-config unless user explicitly enables it
# Usage: make USE_PKG_CONFIG=1
USE_PKG_CONFIG ?= 0

ifeq ($(USE_PKG_CONFIG),1)
  LDLIBS += $(shell pkg-config --libs libavformat libavcodec libavfilter libavutil libswscale libswresample sdl3 SDL3_ttf)
else
  LDLIBS += -lavformat -lavcodec -lavfilter -lavutil -lswscale -lswresample -lSDL3_ttf -lSDL3
endif

src = $(wildcard *.cpp)
obj = $(src:.cpp=.o)
dep = $(obj:.o=.d)
target = video-compare

all: $(target)

$(target): $(obj)
	$(CXX) -o $@ $^ $(LDLIBS)

-include $(dep)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -MF $(@:.o=.d) -c -o $@ $<

test: $(target)
	./$(target) -w 800x screenshot_1.jpg screenshot_2.jpg

.PHONY: clean
clean:
	$(RM) $(obj) $(target) $(dep)

install: $(target)
	install -s video-compare $(BINDIR)

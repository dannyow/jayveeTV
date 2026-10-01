# hostrender — macOS host build of the JayVeeTV render path. No external libs.
#   make            build ./hostrender
#   make check      render the test card once and decode the PNG with sips
#   make clean
FW      := ../../firmware
CXX     := clang++
CC      := clang
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
INC     := -Istubs -I$(FW) -I.

# Channels: every firmware/channels/<id>/<id>.cpp, plus the station list itself
# (channels.cpp) — the harness renders exactly what the board registers.
CH_SRCS   := $(wildcard $(FW)/channels/*/*.cpp) $(FW)/channels/channels.cpp
CORE_SRCS := $(FW)/platform/glitch.cpp $(FW)/platform/tuner.cpp $(FW)/platform/crt.cpp \
             $(FW)/platform/input.cpp $(FW)/platform/arena.cpp $(FW)/platform/sound.cpp \
             $(FW)/platform/remotecard.cpp
C_SRCS    := $(FW)/platform/qrcodegen.c
HOST_SRCS := hostrender.cpp png.cpp channels_host.cpp ch_inputprobe.cpp

BUILD := build
OBJS  := $(patsubst $(FW)/%.cpp,$(BUILD)/fw/%.o,$(CH_SRCS) $(CORE_SRCS)) \
         $(patsubst $(FW)/%.c,$(BUILD)/fw/%.o,$(C_SRCS)) \
         $(patsubst %.cpp,$(BUILD)/host/%.o,$(HOST_SRCS))
DEPS  := $(OBJS:.o=.d)

all: hostrender

hostrender: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD)/fw/%.o: $(FW)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) -MMD -MP -c -o $@ $<

$(BUILD)/fw/%.o: $(FW)/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -O2 -Wall $(INC) -MMD -MP -c -o $@ $<

$(BUILD)/host/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) -MMD -MP -c -o $@ $<

check: hostrender
	./hostrender --channel testcard --frames 1 --out $(BUILD)/check --quiet
	sips -g pixelWidth -g pixelHeight $(BUILD)/check/f000.png

clean:
	rm -rf $(BUILD) hostrender

-include $(DEPS)
.PHONY: all check clean

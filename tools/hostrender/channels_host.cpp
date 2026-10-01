// The harness renders exactly the stations the board has (CHANNELS[] from
// firmware/channels/channels.cpp), plus channels that only exist on the host.
#include "channels_host.h"
#include "channels/channels.h"
#include <string.h>

extern const Channel CH_INPUTPROBE;      // host-only, ch_inputprobe.cpp (shows keys/knob from --input)
static const Channel* const HOST_ONLY[] = { &CH_INPUTPROBE };
static const int HOST_ONLY_COUNT = (int)(sizeof(HOST_ONLY) / sizeof(HOST_ONLY[0]));

int hostChannelCount() { return CHANNEL_COUNT + HOST_ONLY_COUNT; }
const Channel* hostChannelAt(int i) { return i < CHANNEL_COUNT ? CHANNELS[i] : HOST_ONLY[i - CHANNEL_COUNT]; }
const Channel* hostChannel(const char* id) {
  for (int i = 0; i < hostChannelCount(); i++) if (!strcmp(hostChannelAt(i)->info.id, id)) return hostChannelAt(i);
  return nullptr;
}

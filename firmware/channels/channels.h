// Station registry — declared here, defined in channels.cpp. The order in
// channels.cpp IS the station numbering (docs/ARCHITECTURE.md, docs/CHANNELS.md).
#pragma once
#include "platform/channel.h"

extern const Channel* const CHANNELS[];
extern const int CHANNEL_COUNT;

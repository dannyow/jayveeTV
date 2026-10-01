// Channels the host harness knows: the board's own station list (firmware/channels/
// channels.cpp — register a channel there, once) plus host-only channels.
#pragma once
#include "platform/channel.h"

int            hostChannelCount();
const Channel* hostChannelAt(int i);
const Channel* hostChannel(const char* id);   // by ChannelInfo::id, nullptr if unknown

// Station registry. Add a channel: include its header, add one row here — the
// order is the station numbering. See docs/CHANNELS.md.
#include "channels/channels.h"
#include "channels/testcard/testcard.h"
#include "channels/pong/pong.h"

const Channel* const CHANNELS[] = { &CH_TESTCARD, &CH_PONG };
const int CHANNEL_COUNT = (int)(sizeof(CHANNELS) / sizeof(CHANNELS[0]));

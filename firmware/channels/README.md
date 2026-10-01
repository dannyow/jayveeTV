# firmware/channels

A channel is one folder, one programme: it paints pixels, reacts to input and asks for
sounds, and the platform (`firmware/platform/`) makes it look and sound like it's on a
1975 television. Every channel is registered by one line in `channels.cpp`
(`channels.h` declares the registry); the order of that list is the station numbering.

How to write one, the full `Channel` contract and the host-side check are in
[`docs/CHANNELS.md`](../../docs/CHANNELS.md).

## Current channels

| id | what |
|---|---|
| [`testcard`](testcard/README.md) | PM5544-flavoured station-ident card: boot station, `CH_MISBEHAVE` |
| [`pong`](pong/README.md) | 1979 TV-tennis, `CH_GAME`: knob or keys move the paddle |

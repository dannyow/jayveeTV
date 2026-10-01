# Test Card

![Test Card](preview.png)

PM5544-flavoured station-ident test card: grid, big circle, castellations,
colour bars, greyscale, frequency gratings, a wall clock, and `@dannyow` in
the ident band. Pure function of `(x, y)`: no input, no sound of its own
beyond the set's own tone (see below).

## What it is

The boot station. It has `CH_MISBEHAVE`: the set wakes up on it in snow; a
knock tunes it in at once, otherwise it finds the picture by itself after 2 s
(docs/CHANNELS.md). Once locked it
sits there: `CH_ENDLESS`, it never drifts off by itself.

The 1 kHz test tone is the card's own `sound()`: it reads the tuner's
`detune` (passed in on `Sound::detune`) and only speaks near lock, warbling
as the signal wanders; the platform no longer special-cases "is this the
test card" to decide that.

## Controls

None from the phone: the card has an empty panel (`{"blocks":[]}`). Tune,
power, mute and volume are the shell's, not the channel's.

## Credits

Original design and code: this project. `@dannyow` is Daniel's own handle,
not third-party content.

// Arena — one 64 KB block shared by things that never run at the same time
// (a future clip-stream's rings, an emulator's memory image). Static, no heap.
// A claim by a second owner fails loudly instead of silently corrupting the
// first. The array itself is visible (gArena) so a future link-time-addressed
// core (e.g. a Z80) could reach it as a constant; only the current owner may
// touch it. No release-1 channel uses this yet — it moves with the platform
// because the arena, not any one channel, owns the allocation discipline.
#pragma once
#include <stdint.h>
#include <stddef.h>
#define ARENA_SIZE 65536u   // a macro, not a const: the array is declared with C linkage for a future Z80-style core
#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t gArena[ARENA_SIZE];
#ifdef __cplusplus
}
#endif
uint8_t*    arenaClaim(const char* owner, size_t n);   // nullptr: too big, or held by another owner
void        arenaRelease(const char* owner);           // no-op if not the owner
const char* arenaOwner();                              // "" when free

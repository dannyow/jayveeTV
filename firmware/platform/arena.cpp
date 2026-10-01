#include "platform/arena.h"
#include <string.h>
#include <stdio.h>
uint8_t gArena[ARENA_SIZE] __attribute__((aligned(8)));
static const char* s_owner = "";
uint8_t* arenaClaim(const char* owner, size_t n) {
  if (n > ARENA_SIZE) return nullptr;
  if (s_owner[0] && strcmp(s_owner, owner) != 0) { fprintf(stderr, "arena: %s wants %u B but %s holds it\n", owner, (unsigned)n, s_owner); return nullptr; }
  s_owner = owner;
  return gArena;
}
void arenaRelease(const char* owner) { if (strcmp(s_owner, owner) == 0) s_owner = ""; }
const char* arenaOwner() { return s_owner; }

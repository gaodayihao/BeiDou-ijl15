#pragma once

// BUG-005 fix: complete the initialisation the client forgot.
// CMob's constructor never writes the "status latch" at mob+0x528; the pool allocator recycles
// blocks without zeroing them, so a mob can inherit garbage there and then refuses every
// movement-affecting status packet (freeze/stun) for its whole life.
// This hook zeroes the field right after the original constructor runs.
void Hook_CMobCtorLatchFix(bool enable);

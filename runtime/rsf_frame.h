#pragma once
#include "cpu.h"

// Immediately before the game reads its connected controller. Repeated reads
// within the same framework calculation do not open another source frame.
void rsf_frame_input(Cpu *cpu);
// Source attached to the work consumed by this guest draw thread; zero is unknown.
uint64_t rsf_frame_draw_source(Cpu *cpu);
// After the guest queued its output; later work cannot borrow this draw's source.
void rsf_frame_draw_done(Cpu *cpu);

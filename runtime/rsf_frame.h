#pragma once
#include "cpu.h"

// Immediately before the game reads its connected controller. Repeated reads
// within the same framework calculation do not open another source frame.
void rsf_frame_input(Cpu *cpu);

#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct Cpu Cpu;
void items_capture_frame(Cpu *c);
void items_capture_grant(Cpu *c);
void items_sync_tick(Cpu *c, uint64_t sync);
int items_request(const char *actor, int count);
int items_inventory_request(const char *actor);
int items_inventory_ready(void);
int items_supported(void);
int items_busy(void);
void items_status(char *out, size_t capacity);

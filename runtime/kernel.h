// Kernel internals shared by kernel.c, svc.c and the kernel probe (kernel_probe.c). Not part of the host.h contract.
#pragma once
#include "host.h"

// Horizon kernel results (module 1).
#define KERN_R_OUT_OF_RESOURCE 0xCE01u
#define KERN_R_INVALID_STATE_MEM 0xD401u
#define KERN_R_BAD_HANDLE 0xE401u
#define KERN_R_TIMEOUT 0xEA01u
#define KERN_R_CANCELLED 0xEC01u
#define KERN_R_INVALID_ENUM 0xF001u
#define KERN_R_BAD_STATE 0xFA01u
#define KERN_H_CURRENT_THREAD 0xFFFF8000u
#define KERN_WAITERS_BIT 0x40000000u

// Fiber overrides, linked as HOST_OVERRIDE entries in kernel.c.
#define KERN_SYM_INITIALIZE_FIBER "_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi"
#define KERN_SYM_FINALIZE_FIBER "_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE"
#define KERN_SYM_SWITCH_TO_FIBER "_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE"
#define KERN_SYM_GET_CURRENT_FIBER "_ZN2nn2os15GetCurrentFiberEv"
void kern_fiber_initialize(Cpu *c);
void kern_fiber_finalize(Cpu *c);
void kern_fiber_switch(Cpu *c);
void kern_fiber_current(Cpu *c);

uint64_t kern_threads_created(void);

// The guest frame-pointer chain from c->x[29] to stderr.
void kern_backtrace(const Cpu *c);

// ---- probe support
enum { KERN_WAIT_NONE, KERN_WAIT_LOCK, KERN_WAIT_CONDVAR, KERN_WAIT_ADDRESS, KERN_WAIT_SYNC };
#define KERN_THREAD_EXITED 100
int kern_thread_state(uint32_t handle);               // KERN_WAIT_*, KERN_THREAD_EXITED, or -1 for a bad handle
void kern_core_stats(int *held_now, int *held_max);   // virtual-core holders now and the most ever at once
void kern_core_stats_reset(void);
extern volatile int kern_test_cv_old_order;           // WaitProcessWideKeyAtomic: release the lock before setting
                                                      // the key (MNG's pre-fix order, the negative control)
extern void (*volatile kern_test_cv_hook)(void);      // called right after the lock release, holding the kernel lock
extern void (*volatile svc_test_exit_hook)(unsigned code);   // svc 0x07 calls it instead of ExitProcess (probes)

// ---- mod plugin service run (plugin_init.c sets these; the game calls none of the SVCs they serve)
extern uint64_t rt_svc_tick_offset;   // added to GetSystemTick (0x1E): the service run's second run picks other ticks
extern void (*rt_plugin_svc)(Cpu *c, uint32_t imm, uint64_t pc);   // with RT_TRACE_PLUGIN: sees each SVC first
extern uint32_t (*rt_process_memory)(int unmap, uint64_t dst, uint32_t process, uint64_t src, uint64_t size);
                                      // Map/UnmapProcessMemory (0x74/0x75); NULL outside the service run: fatal

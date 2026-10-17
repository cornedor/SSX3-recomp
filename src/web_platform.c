/*
 * Pieces the web build needs that another source provides natively.
 */
#include <stdint.h>

#if !defined(SSX3_VULKAN)
/* Slow-frame catcher (ssx3_overrides.c): the Vulkan backend stamps these at
 * each flip. Without it they stay 0, which the catcher reads as "unknown". */
volatile uint32_t ssx3_last_flip_ms, ssx3_exec_tid, ssx3_flip_tid;
#endif

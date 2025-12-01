#pragma once
#include <stdbool.h>

#include "core.h"
#include "types.h"

// DO NOT MODIFY THIS STRUCT CARELESSLY
// THIS IS ACCESSED AS RAW MEMORY FROM WASM
// (this is really ugly, i know)
// possible fix: sizeof and offsetof
// and expose them to JS
// but that is quite verbose
typedef struct {
    u32 pc;       // for backtrace view
    u32 sp;       // for backtrace view
    u32 args[8];  // for backtrace view

    u32 sregs[12];
    u32 ra;
    u32 reg_bitmap;
} ShadowStackEnt;

ARES_ARRAY_TYPE(ShadowStackEnt);

void callsan_init();
void callsan_store(int reg);
void callsan_call();
bool callsan_ret();
bool callsan_can_load(int reg);
void callsan_report_store(u32 addr, u32 size, int reg);
bool callsan_check_load(u32 addr, u32 size);

extern u32 g_reg_bitmap;
extern ARES_ARRAY(ShadowStackEnt) g_shadow_stack;
extern u8 g_callsan_stack_written_by[];

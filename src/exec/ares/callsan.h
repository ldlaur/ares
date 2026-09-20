#pragma once
#include <stdbool.h>

#include "core.h"
#include "types.h"

void callsan_init(AresState *g);
void callsan_store(AresState *g, int reg);
void callsan_call(AresState *g);
bool callsan_ret(AresState *g);
bool callsan_can_load(AresState *g, int reg);
bool callsan_check_sp(AresState *g);
void callsan_report_store(AresState *g, u32 addr, u32 size, int reg);
bool callsan_check_load(AresState *g, u32 addr, u32 size);

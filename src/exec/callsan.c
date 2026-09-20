#include "ares/callsan.h"

#include "ares/core.h"
#include "ares/emulate.h"

void callsan_init(AresState *g) {
    g->callsan_on = true;
    memset(g->callsan_stack_written_by, 0xFF,
           sizeof(g->callsan_stack_written_by));
    g->reg_bitmap = (1ul << REG_ZERO) | (1ul << REG_SP) | (1ul << REG_TP) |
                    (1ul << REG_GP) | (1u << REG_FP) | (1u << REG_S1) |
                    (1u << REG_S2) | (1u << REG_S3) | (1u << REG_S4) |
                    (1u << REG_S5) | (1u << REG_S6) | (1u << REG_S7) |
                    (1u << REG_S8) | (1u << REG_S9) | (1u << REG_S10) |
                    (1u << REG_S11);
    g->shadow_stack = ARES_ARRAY_NEW(ShadowStackEnt);
    g->reg_bitmap_ever_written = 0;
}

bool callsan_can_load(AresState *g, int reg) {
    if (!g->callsan_on) return true;
    if (reg == 0) return true;
    if (((g->reg_bitmap >> reg) & 1) == 0) {
        // there are still two causes:
        // it could be caused by accessing an uninitialized register (*never*
        // written) or by a register clobbered by the call
        g->runtime_error_params[0] = reg;
        if (((g->reg_bitmap_ever_written >> reg) & 1) == 0)
            g->runtime_error_type = ERROR_CALLSAN_CANTREAD;
        else g->runtime_error_type = ERROR_CALLSAN_CALL_CLOBBERED;
        return false;
    }
    return true;
}

void callsan_store(AresState *g, int reg) {
    if (!g->callsan_on) return;
    g->reg_bitmap |= 1u << reg;
    g->reg_bitmap_ever_written |= 1u << reg;
}

bool callsan_check_sp(AresState *g) {
    if (!g->callsan_on) return true;
    u32 sp = g->regs[REG_SP];
    if (sp >= STACK_TOP - STACK_LEN && sp <= STACK_TOP) return true;
    g->runtime_error_type = ERROR_CALLSAN_SP_INVALID;
    g->runtime_error_params[0] = sp;
    return false;
}

const u32 CALLSAN_CALL_ACCESSIBLE =
    (1ul << REG_ZERO) | (1ul << REG_SP) | (1ul << REG_RA) | (1ul << REG_TP) |
    (1ul << REG_GP) | (1u << REG_A0) | (1u << REG_A1) | (1u << REG_A2) |
    (1u << REG_A3) | (1u << REG_A4) | (1u << REG_A5) | (1u << REG_A6) |
    (1u << REG_A7) | (1u << REG_FP) | (1u << REG_S1) | (1u << REG_S2) |
    (1u << REG_S3) | (1u << REG_S4) | (1u << REG_S5) | (1u << REG_S6) |
    (1u << REG_S7) | (1u << REG_S8) | (1u << REG_S9) | (1u << REG_S10) |
    (1u << REG_S11);

// *must* exclude A0 and A1 to allow return value passing
// (A1 for 2-reg returns)
const u32 CALLSAN_CALL_CLOBBERED =
    (1u << REG_T0) | (1u << REG_T1) | (1u << REG_T2) | (1u << REG_T3) |
    (1u << REG_T4) | (1u << REG_T5) | (1u << REG_T6) | (1u << REG_A2) |
    (1u << REG_A3) | (1u << REG_A4) | (1u << REG_A5) | (1u << REG_A6) |
    (1u << REG_A7);

void callsan_call(AresState *g) {
    if (!g->callsan_on) return;
    if (!callsan_check_sp(g)) return;
    ShadowStackEnt *e = ARES_ARRAY_PUSH(&g->shadow_stack);
    e->sregs[0] = g->regs[REG_FP];
    e->sregs[1] = g->regs[REG_S1];
    for (int i = REG_S2; i <= REG_S11; i++)
        e->sregs[2 + i - REG_S2] = g->regs[i];
    for (int i = REG_A0; i <= REG_A7; i++) e->args[i - REG_A0] = g->regs[i];
    e->sp = g->regs[REG_SP];
    e->pc = g->pc;
    e->ra = g->regs[REG_RA];
    e->reg_bitmap = g->reg_bitmap;
    // only call accessible registers can be read after the call
    // &= and not = because they still must have been written to before
    g->reg_bitmap &= CALLSAN_CALL_ACCESSIBLE;
}

bool callsan_ret(AresState *g) {
    if (!g->callsan_on) return true;
    if (ARES_ARRAY_LEN(&g->shadow_stack) == 0) {
        g->runtime_error_type = ERROR_CALLSAN_RET_EMPTY;
        return false;
    }

    ShadowStackEnt *e = ARES_ARRAY_POP(&g->shadow_stack);

    if (g->regs[REG_SP] != e->sp) {
        g->runtime_error_type = ERROR_CALLSAN_SP_MISMATCH;
        g->runtime_error_params[1] = e->sp;
        return false;
    }
    if (!callsan_check_sp(g)) return false;

    if (g->regs[REG_RA] != e->ra) {
        g->runtime_error_type = ERROR_CALLSAN_RA_MISMATCH;
        g->runtime_error_params[1] = e->ra;
        return false;
    }

    u32 sregs[12];
    sregs[0] = g->regs[REG_FP];
    sregs[1] = g->regs[REG_S1];
    for (int i = 0; i < 10; i++) sregs[2 + i] = g->regs[18 + i];
    for (int i = 0; i < 12; i++) {
        if (sregs[i] != e->sregs[i]) {
            g->runtime_error_type = ERROR_CALLSAN_NOT_SAVED;
            if (i == 0) g->runtime_error_params[0] = REG_FP;
            else if (i == 1) g->runtime_error_params[0] = REG_S1;
            else g->runtime_error_params[0] = REG_S2 + (i - 2);
            g->runtime_error_params[1] = e->sregs[i];
            return false;
        }
    }

    // after a function return you cannot read the A (except A0 and A1) and T
    // registers since the function hypothetically may have clobbered them
    u32 a0_a1_set = g->reg_bitmap & ((1ul << REG_A0) | (1ul << REG_A1));
    g->reg_bitmap = (e->reg_bitmap & ~CALLSAN_CALL_CLOBBERED) | a0_a1_set;

    // rest of the stack is all poisoned
    u32 endidx = (e->sp - (STACK_TOP - STACK_LEN)) / 4;
    for (u32 i = 0; i < endidx; i++) g->callsan_stack_written_by[i] = -1;
    return true;
}

void callsan_report_store(AresState *g, u32 addr, u32 size, int reg) {
    if (!g->callsan_on) return;
    bool in_stack = addr >= STACK_TOP - STACK_LEN && addr < STACK_TOP &&
                    size != 0 && size <= STACK_TOP - addr;
    if (!in_stack) return;
    u32 off = addr - (STACK_TOP - STACK_LEN);
    u32 startidx = off / 4;
    u32 endidx = (off + size - 1) / 4;
    g->callsan_stack_written_by[startidx] = reg;
    if (endidx != startidx) g->callsan_stack_written_by[endidx] = reg;
}

bool callsan_check_load(AresState *g, u32 addr, u32 size) {
    if (!g->callsan_on) return true;
    bool in_stack = addr >= STACK_TOP - STACK_LEN && addr < STACK_TOP &&
                    size != 0 && size <= STACK_TOP - addr;
    if (!in_stack) return true;
    u32 off = addr - (STACK_TOP - STACK_LEN);
    u32 startidx = off / 4;
    u32 endidx = (off + size - 1) / 4;
    return g->callsan_stack_written_by[startidx] != 0xFF &&
           g->callsan_stack_written_by[endidx] != 0xFF;
}

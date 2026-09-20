#pragma once

#include "core.h"

#define PRIV_MACHINE 3
#define PRIV_SUPERVISOR 1
#define PRIV_USER 0

// Interrupt bit mask (MSB)
#define CAUSE_INTERRUPT (1ul << 31)

// Exception codes (INT = 0)
#define CAUSE_INST_ADDR_MISALIGNED 0x00
#define CAUSE_INST_ACCESS_FAULT 0x01
#define CAUSE_ILLEGAL_INSTRUCTION 0x02
#define CAUSE_BREAKPOINT 0x03
#define CAUSE_LOAD_ADDR_MISALIGNED 0x04
#define CAUSE_LOAD_ACCESS_FAULT 0x05
#define CAUSE_STORE_ADDR_MISALIGNED 0x06
#define CAUSE_STORE_ACCESS_FAULT 0x07
#define CAUSE_U_ECALL 0x08
#define CAUSE_S_ECALL 0x09
#define CAUSE_VS_ECALL 0x0A
#define CAUSE_M_ECALL 0x0B
#define CAUSE_INST_PAGE_FAULT 0x0C
#define CAUSE_LOAD_PAGE_FAULT 0x0D
#define CAUSE_STORE_PAGE_FAULT 0x0F

// Interrupt codes (INT = 1)
#define CAUSE_SUPERVISOR_SOFTWARE (CAUSE_INTERRUPT | 1)
#define CAUSE_MACHINE_SOFTWARE (CAUSE_INTERRUPT | 3)
#define CAUSE_SUPERVISOR_TIMER (CAUSE_INTERRUPT | 5)
#define CAUSE_MACHINE_TIMER (CAUSE_INTERRUPT | 7)
#define CAUSE_SUPERVISOR_EXTERNAL (CAUSE_INTERRUPT | 9)
#define CAUSE_MACHINE_EXTERNAL (CAUSE_INTERRUPT | 11)

extern export u32 g_regs[32];
extern export u32 g_csr[4096];
extern export u32 g_pc;

extern export u32 g_runtime_error_params[2];
extern export Error g_runtime_error_type;

void emulator_enter_kernel(AresState *g);
void emulator_leave_kernel(AresState *g);
u32 LOAD(AresState *g, u32 addr, int size, bool *err);
void STORE(AresState *g, u32 addr, u32 val, int size, bool *err);
void emulator_deliver_interrupt(AresState *g, u32 cause);
void emulator_init(AresState *g);
void emulator_exit(AresState *g, int code);
void emulator_interrupt_set_pending(AresState *g, u32 intno);
void emulator_interrupt_clear_pending(AresState *g, u32 intno);
size_t disassemble(u32 inst, char *buf, size_t buflen);

typedef struct PcrelHiReloc {
    u32 label_addr;
    u32 dest_addr;
} PcrelHiReloc;
ARES_ARRAY_TYPE(PcrelHiReloc);

typedef struct {
    u32 pc;       // for backtrace view
    u32 sp;       // for backtrace view
    u32 args[8];  // for backtrace view

    u32 sregs[12];
    u32 ra;
    u32 reg_bitmap;
} ShadowStackEnt;

ARES_ARRAY_TYPE(ShadowStackEnt);

typedef struct AresState {
    Section *text, *data, *stack, *kernel_text, *kernel_data, *mmio;

    ARES_ARRAY(SectionPtr) sections;
    ARES_ARRAY(Extern) externs;
    ARES_ARRAY(LabelData) labels;
    ARES_ARRAY(Global) globals;
    ARES_ARRAY(LocalLabel) local_labels;
    ARES_ARRAY(PcrelHiReloc) pcrel_hi_relocs;

    ARES_ARRAY(DeferredInsn) deferred_insn;

    Section *section;

    bool in_fixup;
    u32 error_line;
    const char *error;

    u32 runtime_error_params[2];
    Error runtime_error_type;

    bool allow_externs;

    u32 reg_bitmap;
    u32 reg_bitmap_ever_written;
    ARES_ARRAY(ShadowStackEnt) shadow_stack;
    u8 callsan_stack_written_by[STACK_LEN / 4];
    bool callsan_on;

    u32 regs[32];
    u32 csr[4096];
    u32 pc;

    u32 mem_written_len;
    u32 mem_written_addr;
    u32 reg_written;

    bool exited;
    int exit_code;

    u32 got_breakpoint;

    int privilege_level;
} AresState;

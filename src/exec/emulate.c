#include "ares/emulate.h"

#include "ares/callsan.h"
#include "ares/core.h"
#include "ares/dev.h"

export u32 g_regs[32];
export u32 g_csr[4096];
export u32 g_pc;

export u32 g_mem_written_len;
export u32 g_mem_written_addr;
export u32 g_reg_written;

export bool g_exited;
export int g_exit_code;

// u32 for WASM interop
export u32 g_got_breakpoint;

extern u32 g_runtime_error_params[2];
extern Error g_runtime_error_type;

static int g_privilege_level = PRIV_USER;

static size_t i32_to_str(i32 val, char buf[12]);

static inline i32 sext(u32 x, int bits) {
    int m = 32 - bits;
    return ((i32)(x << m)) >> m;
}

// Taken from Fabrice Bellard's TinyEMU
static inline i32 div32(i32 a, i32 b) {
    if (b == 0) {
        return -1;
    } else if (a == (i32)(1ul << 31) && b == -1) {
        return a;
    } else {
        return a / b;
    }
}
static inline u32 divu32(u32 a, u32 b) {
    if (b == 0) {
        return -1;
    } else {
        return a / b;
    }
}
static inline i32 rem32(i32 a, i32 b) {
    if (b == 0) {
        return a;
    } else if (a == (i32)(1ul << 31) && b == -1) {
        return 0;
    } else {
        return a % b;
    }
}
static inline u32 remu32(u32 a, u32 b) {
    if (b == 0) {
        return a;
    } else {
        return a % b;
    }
}

Section *emulator_get_section(u32 addr) {
    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_sections); i++) {
        Section *sec = *ARES_ARRAY_GET(&g_sections, i);
        if (addr >= sec->base && addr < sec->limit) return sec;
    }
    return NULL;
}

u8 *emulator_get_addr(u32 addr, int size, Section **out_sec) {
    Section *addr_sec = emulator_get_section(addr);
    if (out_sec) *out_sec = addr_sec;
    if (!addr_sec) return NULL;
    u32 end;
    if (__builtin_add_overflow(addr, size, &end)) return NULL;
    // NOTE: addr+size is one over the end of the accessed region
    // so it is correct for it to be > and not >=
    if (end > addr_sec->contents.len + addr_sec->base) return NULL;
    return addr_sec->contents.buf + (addr - addr_sec->base);
}

u32 LOAD(u32 addr, int size, bool *err) {
    Section *mem_sec;
    u8 *mem = emulator_get_addr(addr, size, &mem_sec);

    if (!mem_sec || !mem_sec->read ||
        (mem_sec->super && g_privilege_level == PRIV_USER)) {
        *err = true;
        return 0;
    }

    if (mem_sec->base == MMIO_BASE) {
        u32 ret = 0;
        *err = !mmio_read(addr - MMIO_BASE, size, &ret);
        return ret;
    } else if (!mem) {
        *err = true;
        return 0;
    }

    u32 ret = 0;
    if (size == 1) {
        ret = mem[0];
    } else if (size == 2) {
        ret = mem[0];
        ret |= ((u32)mem[1]) << 8;
    } else if (size == 4) {
        ret = mem[0];
        ret |= ((u32)mem[1]) << 8;
        ret |= ((u32)mem[2]) << 16;
        ret |= ((u32)mem[3]) << 24;
    } else assert(!"Invalid size");
    *err = false;
    return ret;
}

void STORE(u32 addr, u32 val, int size, bool *err) {
    g_mem_written_len = size;
    g_mem_written_addr = addr;

    Section *mem_sec;
    u8 *mem = emulator_get_addr(addr, size, &mem_sec);

    if (!mem_sec || !mem_sec->write ||
        (mem_sec->super && g_privilege_level == PRIV_USER)) {
        *err = true;
        return;
    }

    if (mem_sec->base == MMIO_BASE) {
        *err = !mmio_write(addr - MMIO_BASE, size, val);
        return;
    } else if (!mem) {
        *err = true;
        return;
    }

    if (size == 1) {
        mem[0] = val;
    } else if (size == 2) {
        mem[0] = val;
        mem[1] = val >> 8;
    } else if (size == 4) {
        mem[0] = val;
        mem[1] = val >> 8;
        mem[2] = val >> 16;
        mem[3] = val >> 24;
    } else assert(!"Invalid size");
    *err = false;
}

// ebreak only does a breakpoint if the emulator caller knows about it
// otherwise, it does nothing to allow runs to complete
void do_ebreak(void) {
    g_got_breakpoint = 1;
    g_pc += 4;
}

void do_syscall(void) {
    u32 scause = CAUSE_U_ECALL;
    if (g_privilege_level == PRIV_SUPERVISOR) scause = CAUSE_S_ECALL;

    if (g_kernel_text && !ARES_ARRAY_IS_EMPTY(&g_kernel_text->contents)) {
        emulator_deliver_interrupt(scause);
        return;
    }

    g_reg_written = 0;

    u32 param = g_regs[10];
    if (g_regs[17] == 1) {
        // print int
        char buffer[12];
        size_t len = i32_to_str((i32)g_regs[10], buffer);
        for (size_t i = 0; i < len; i++) putchar(buffer[i]);
    } else if (g_regs[17] == 4) {
        // print string
        u32 i = 0;
        while (1) {
            bool err = false;
            u8 ch = LOAD(param + i, 1, &err);
            if (err) return;  // TODO: return an error?
            if (ch == 0) break;
            i++;
            putchar(ch);
        }
    } else if (g_regs[17] == 11) {
        // print char
        putchar(param);
    } else if (g_regs[17] == 34) {
        // print int hex
        putchar('0');
        putchar('x');
        for (int i = 32 - 4; i >= 0; i -= 4)
            putchar("0123456789abcdef"[(param >> i) & 15]);
    } else if (g_regs[17] == 35) {
        // print int binary
        putchar('0');
        putchar('b');
        for (int i = 31; i >= 0; i--) {
            putchar(((param >> i) & 1) ? '1' : '0');
        }
    } else if (g_regs[17] == 93 || g_regs[17] == 7 || g_regs[17] == 10) {
        emu_exit();
    } else {
        g_runtime_error_params[0] = g_regs[17];
        g_runtime_error_type = ERROR_INVALID_ECALL;
        return;
    }

    g_pc += 4;
}

void do_sret(void) {
    // SRET is only legal in supervisor
    if (g_privilege_level != PRIV_SUPERVISOR) {
        g_runtime_error_params[0] = g_pc;
        g_runtime_error_type = ERROR_UNHANDLED_INSN;
        return;
    }
    u32 status = g_csr[CSR_MSTATUS];
    bool old_spp = status & STATUS_SPP;
    bool old_spie = status & STATUS_SPIE;
    // SIE = SPIE
    status = (status & ~STATUS_SIE) | (old_spie ? STATUS_SIE : 0);
    // SPIE = 1
    status |= STATUS_SPIE;
    // SPP = 0
    status &= ~STATUS_SPP;
    g_csr[CSR_MSTATUS] = status;
    g_privilege_level = old_spp;
    g_pc = g_csr[CSR_SEPC];
}

// TODO: trap invalid CSRs
// and make unimplemented features read-only

#define SSTATUS_MASK (STATUS_SIE | STATUS_SPIE | STATUS_SPP | STATUS_FS_MASK)
#define SUPERVISOR_INT_MASK ((1 << 1) | (1 << 5) | (1 << 9))

u32 rdcsr(u32 csr) {
    u32 mask = -1u;
    if (csr == _CSR_SSTATUS) csr = CSR_MSTATUS, mask = SSTATUS_MASK;
    else if (csr == _CSR_SIE) csr = CSR_MIE, mask = SUPERVISOR_INT_MASK;
    else if (csr == _CSR_SIP) csr = CSR_MIP, mask = SUPERVISOR_INT_MASK;
    return g_csr[csr] & mask;
}

void wrcsr(u32 csr, u32 val) {
    // for SIP, only SSIP (software interrupts) is writable
    // since it is the way to EOI a software interrupt
    // whereas the other ones are EOI'd by the respective devices
    u32 mask = -1u;
    if (csr == _CSR_SSTATUS) csr = CSR_MSTATUS, mask = SSTATUS_MASK;
    else if (csr == _CSR_SIE) csr = CSR_MIE, mask = SUPERVISOR_INT_MASK;
    else if (csr == _CSR_SIP)
        csr = CSR_MIP,
        mask = 1u << (CAUSE_SUPERVISOR_SOFTWARE & ~CAUSE_INTERRUPT);
    g_csr[csr] = (g_csr[csr] & ~mask) | (val & mask);
}

// helpers
static inline u32 c_reg(u32 reg) { return 8 + reg; }

static inline i32 c_imm6(u16 inst) {
    return sext((extr(inst, 12, 12) << 5) | extr(inst, 6, 2), 6);
}

static inline u32 c_lwsp_off(u16 inst) {
    return (extr(inst, 12, 12) << 5) | (extr(inst, 6, 4) << 2) |
           (extr(inst, 3, 2) << 6);
}

static inline u32 c_swsp_off(u16 inst) {
    return (extr(inst, 12, 9) << 2) | (extr(inst, 8, 7) << 6);
}

static inline u32 c_lw_sw_off(u16 inst) {
    return (extr(inst, 12, 10) << 3) | (extr(inst, 6, 6) << 2) |
           (extr(inst, 5, 5) << 6);
}

static inline u32 c_addi4spn_nzuimm(u16 inst) {
    return (extr(inst, 12, 11) << 4) | (extr(inst, 10, 7) << 6) |
           (extr(inst, 6, 6) << 2) | (extr(inst, 5, 5) << 3);
}

static inline i32 c_addi16sp_nzimm(u16 inst) {
    return sext((extr(inst, 12, 12) << 9) | (extr(inst, 6, 6) << 4) |
                    (extr(inst, 5, 5) << 6) | (extr(inst, 4, 3) << 7) |
                    (extr(inst, 2, 2) << 5),
                10);
}

static inline i32 c_jump_off(u16 inst) {
    return sext((extr(inst, 12, 12) << 11) | (extr(inst, 11, 11) << 4) |
                    (extr(inst, 10, 9) << 8) | (extr(inst, 8, 8) << 10) |
                    (extr(inst, 7, 7) << 6) | (extr(inst, 6, 6) << 7) |
                    (extr(inst, 5, 3) << 1) | (extr(inst, 2, 2) << 5),
                12);
}

static inline i32 c_branch_off(u16 inst) {
    return sext((extr(inst, 12, 12) << 8) | (extr(inst, 11, 10) << 3) |
                    (extr(inst, 6, 5) << 6) | (extr(inst, 4, 3) << 1) |
                    (extr(inst, 2, 2) << 5),
                9);
}

static bool c_load_word(u32 rd, u32 rs1, u32 off) {
    bool err = false;
    if (!callsan_can_load(rs1)) return true;

    u32 addr = g_regs[rs1] + off;
    g_regs[rd] = LOAD(addr, 4, &err);
    if (err) {
        g_runtime_error_params[0] = addr;
        g_runtime_error_type = ERROR_LOAD;
        return true;
    }

    if (!callsan_check_load(addr, 4)) {
        g_runtime_error_params[0] = addr;
        g_runtime_error_type = ERROR_CALLSAN_LOAD_STACK;
        return true;
    }

    g_pc += 2;
    g_reg_written = rd;
    callsan_store(rd);
    return true;
}

static bool c_store_word(u32 rs2, u32 rs1, u32 off) {
    bool err = false;
    if (!callsan_can_load(rs1)) return true;
    if (!callsan_can_load(rs2)) return true;

    u32 addr = g_regs[rs1] + off;
    STORE(addr, g_regs[rs2], 4, &err);
    if (err) {
        g_runtime_error_params[0] = addr;
        g_runtime_error_type = ERROR_STORE;
        return true;
    }

    callsan_report_store(addr, 4, rs2);
    g_pc += 2;
    return true;
}

static bool emulate_compressed(u16 inst) {
    u32 opcode = extr(inst, 1, 0);
    u32 funct3 = extr(inst, 15, 13);

    if (opcode == 0b00) {
        if (funct3 == 0b000) {  // c.addi4spn
            u32 rd = c_reg(extr(inst, 4, 2));
            u32 nzuimm = c_addi4spn_nzuimm(inst);
            if (nzuimm == 0) return false;
            if (!callsan_can_load(REG_SP)) return true;

            g_regs[rd] = g_regs[REG_SP] + nzuimm;
            g_pc += 2;
            g_reg_written = rd;
            callsan_store(rd);
            return true;
        }
        if (funct3 == 0b010) {  // c.lw
            return c_load_word(c_reg(extr(inst, 4, 2)), c_reg(extr(inst, 9, 7)),
                               c_lw_sw_off(inst));
        }
        if (funct3 == 0b110) {  // c.sw
            return c_store_word(c_reg(extr(inst, 4, 2)),
                                c_reg(extr(inst, 9, 7)), c_lw_sw_off(inst));
        }
        return false;
    }

    if (opcode == 0b01) {
        if (funct3 == 0b000) {  // c.addi / c.nop
            u32 rd = extr(inst, 11, 7);
            i32 nzimm = c_imm6(inst);
            if (rd == 0) {
                if (nzimm != 0) return false;
                g_pc += 2;
                return true;
            }
            if (nzimm == 0) return false;
            if (!callsan_can_load(rd)) return true;

            g_regs[rd] += nzimm;
            g_pc += 2;
            g_reg_written = rd;
            callsan_store(rd);
            return true;
        }
        if (funct3 == 0b001) {  // c.jal
            g_regs[REG_RA] = g_pc + 2;
            g_reg_written = REG_RA;
            callsan_store(REG_RA);
            g_pc += c_jump_off(inst);
            callsan_call();
            return true;
        }
        if (funct3 == 0b010) {  // c.li
            u32 rd = extr(inst, 11, 7);
            if (rd == 0) return false;

            g_regs[rd] = c_imm6(inst);
            g_pc += 2;
            g_reg_written = rd;
            callsan_store(rd);
            return true;
        }
        if (funct3 == 0b011) {  // c.addi16sp / c.lui
            u32 rd = extr(inst, 11, 7);
            if (rd == REG_SP) {
                i32 nzimm = c_addi16sp_nzimm(inst);
                if (nzimm == 0) return false;
                if (!callsan_can_load(REG_SP)) return true;

                g_regs[REG_SP] += nzimm;
                g_pc += 2;
                g_reg_written = REG_SP;
                callsan_store(REG_SP);
                return true;
            }

            i32 nzimm = c_imm6(inst);
            if (rd == 0 || nzimm == 0) return false;
            g_regs[rd] = (u32)(nzimm << 12);
            g_pc += 2;
            g_reg_written = rd;
            callsan_store(rd);
            return true;
        }
        if (funct3 == 0b100) {
            u32 funct2 = extr(inst, 11, 10);
            u32 rd = c_reg(extr(inst, 9, 7));

            if (funct2 == 0b00 || funct2 == 0b01) {  // c.srli / c.srai
                if (extr(inst, 12, 12) != 0) return false;
                u32 shamt = extr(inst, 6, 2);
                if (shamt == 0) return false;
                if (!callsan_can_load(rd)) return true;

                if (funct2 == 0b00) g_regs[rd] >>= shamt;
                else g_regs[rd] = (u32)((i32)g_regs[rd] >> shamt);
                g_pc += 2;
                g_reg_written = rd;
                callsan_store(rd);
                return true;
            }
            if (funct2 == 0b10) {  // c.andi
                if (!callsan_can_load(rd)) return true;

                g_regs[rd] &= c_imm6(inst);
                g_pc += 2;
                g_reg_written = rd;
                callsan_store(rd);
                return true;
            }
            if (funct2 == 0b11) {  // c.sub / c.xor / c.or / c.and
                if (extr(inst, 12, 12) != 0) return false;
                u32 rs2 = c_reg(extr(inst, 4, 2));
                u32 op = extr(inst, 6, 5);
                if (!callsan_can_load(rd)) return true;
                if (!callsan_can_load(rs2)) return true;

                if (op == 0b00) g_regs[rd] -= g_regs[rs2];       // c.sub
                else if (op == 0b01) g_regs[rd] ^= g_regs[rs2];  // c.xor
                else if (op == 0b10) g_regs[rd] |= g_regs[rs2];  // c.or
                else g_regs[rd] &= g_regs[rs2];                  // c.and
                g_pc += 2;
                g_reg_written = rd;
                callsan_store(rd);
                return true;
            }
            return false;
        }
        if (funct3 == 0b101) {  // c.j
            g_pc += c_jump_off(inst);
            return true;
        }
        if (funct3 == 0b110 || funct3 == 0b111) {  // c.beqz / c.bnez
            u32 rs1 = c_reg(extr(inst, 9, 7));
            if (!callsan_can_load(rs1)) return true;

            bool take = g_regs[rs1] == 0;
            if (funct3 == 0b111) take = !take;
            g_pc += take ? c_branch_off(inst) : 2;
            return true;
        }
        return false;
    }

    if (opcode == 0b10) {
        if (funct3 == 0b000) {  // c.slli
            if (extr(inst, 12, 12) != 0) return false;
            u32 rd = extr(inst, 11, 7);
            u32 shamt = extr(inst, 6, 2);
            if (rd == 0 || shamt == 0) return false;
            if (!callsan_can_load(rd)) return true;

            g_regs[rd] <<= shamt;
            g_pc += 2;
            g_reg_written = rd;
            callsan_store(rd);
            return true;
        }
        if (funct3 == 0b010) {  // c.lwsp
            u32 rd = extr(inst, 11, 7);
            if (rd == 0) return false;
            return c_load_word(rd, REG_SP, c_lwsp_off(inst));
        }
        if (funct3 == 0b100) {
            u32 rd = extr(inst, 11, 7);
            u32 rs2 = extr(inst, 6, 2);

            if (extr(inst, 12, 12) == 0) {
                if (rs2 == 0) {  // c.jr
                    if (rd == 0) return false;
                    if (!callsan_can_load(rd)) return true;
                    if (rd == REG_RA && !callsan_ret()) return true;
                    g_pc = g_regs[rd] & ~1u;
                    return true;
                }
                if (rd == 0) return false;
                if (!callsan_can_load(rs2)) return true;

                g_regs[rd] = g_regs[rs2];
                g_pc += 2;
                g_reg_written = rd;
                callsan_store(rd);
                return true;
            }

            if (rs2 == 0) {
                if (rd == 0) {  // c.ebreak
                    g_got_breakpoint = 1;
                    g_pc += 2;
                    return true;
                }
                if (!callsan_can_load(rd)) return true;
                u32 target = g_regs[rd] & ~1u;

                g_regs[REG_RA] = g_pc + 2;
                g_reg_written = REG_RA;
                callsan_store(REG_RA);
                g_pc = target;
                callsan_call();
                return true;
            }

            if (rd == 0) return false;
            if (!callsan_can_load(rd)) return true;
            if (!callsan_can_load(rs2)) return true;

            g_regs[rd] += g_regs[rs2];
            g_pc += 2;
            g_reg_written = rd;
            callsan_store(rd);
            return true;
        }
        if (funct3 == 0b110) {  // c.swsp
            return c_store_word(extr(inst, 6, 2), REG_SP, c_swsp_off(inst));
        }
        return false;
    }

    return false;
}

void emulate(void) {
    g_runtime_error_type = ERROR_NONE;
    g_mem_written_len = 0;
    g_reg_written = 0;
    g_regs[0] = 0;
    g_got_breakpoint = 0;
    bool err;

    if (g_privilege_level == PRIV_USER || (g_csr[CSR_MSTATUS] & STATUS_SIE)) {
        u32 pending = g_csr[CSR_MIP] & g_csr[CSR_MIE];
        if (pending != 0) {
            int intno = __builtin_ctz(pending);
            emulator_deliver_interrupt(CAUSE_INTERRUPT | intno);
        }
    }

    u32 halfword = LOAD(g_pc, 2, &err);
    if (err) {
        g_runtime_error_params[0] = g_pc;
        g_runtime_error_type = ERROR_FETCH;
        return;
    }

    // compressed instructions are 16-bit and must be handled separately
    if ((halfword & 0b11) != 0b11) {
        if (!emulate_compressed((u16)halfword)) {
            g_runtime_error_params[0] = g_pc;
            g_runtime_error_type = ERROR_UNHANDLED_INSN;
        }
        return;
    }

    u32 inst = LOAD(g_pc, 4, &err);
    if (err) {
        g_runtime_error_params[0] = g_pc;
        g_runtime_error_type = ERROR_FETCH;
        return;
    }

    u32 rd = extr(inst, 11, 7);
    u32 rs1 = extr(inst, 19, 15);
    u32 rs2 = extr(inst, 24, 20);
    u32 funct7 = extr(inst, 31, 25);
    u32 funct3 = extr(inst, 14, 12);

    i32 btype = sext((extr(inst, 31, 31) << 12) | (extr(inst, 7, 7) << 11) |
                         (extr(inst, 30, 25) << 5) | (extr(inst, 11, 8) << 1),
                     13);
    i32 stype = sext((extr(inst, 31, 25) << 5) | (extr(inst, 11, 7)), 12);
    i32 jtype = sext((extr(inst, 31, 31) << 20) | (extr(inst, 19, 12) << 12) |
                         (extr(inst, 20, 20) << 11) | (extr(inst, 30, 21) << 1),
                     21);
    i32 itype = sext(extr(inst, 31, 20), 12);
    i32 utype = extr(inst, 31, 12) << 12;

    u32 S1 = g_regs[rs1];
    u32 S2 = g_regs[rs2];
    u32 *D = &g_regs[rd];

    u32 opcode = extr(inst, 6, 0);

    // LUI
    if (opcode == 0b0110111) {
        *D = utype;
        g_pc += 4;
        g_reg_written = rd;
        callsan_store(rd);
        return;
    }

    // AUIPC
    if (opcode == 0b0010111) {
        *D = g_pc + utype;
        g_pc += 4;
        g_reg_written = rd;
        callsan_store(rd);
        return;
    }

    // JAL
    if (opcode == 0b1101111) {
        *D = g_pc + 4;
        g_pc += jtype;
        g_reg_written = rd;
        callsan_store(rd);
        if (rd == 1) callsan_call();
        return;
    }

    // JALR
    if (opcode == 0b1100111) {
        if (!callsan_can_load(rs1)) return;
        callsan_store(rd);
        *D = g_pc + 4;
        // this has to be checked before updating pc so that the highlighted pc
        // is correct
        if (rd == 0 && rs1 == 1) {  // jr ra/ret
            if (!callsan_ret()) return;
        }
        g_pc = (S1 + itype) & ~1;
        if (rd == 1) callsan_call();
        g_reg_written = rd;
        return;
    }

    // BEQ/BNE/BLT/BGE/BLTU/BGEU
    if (opcode == 0b1100011) {
        if (!callsan_can_load(rs1)) return;
        if (!callsan_can_load(rs2)) return;
        bool T = false;
        if ((funct3 >> 1) == 0) T = S1 == S2;                // EQ/NE
        else if ((funct3 >> 1) == 2) T = (i32)S1 < (i32)S2;  // LT/GE
        else if ((funct3 >> 1) == 3) T = S1 < S2;            // LTU/GEU
        else {
            g_runtime_error_params[0] = g_pc;
            g_runtime_error_type = ERROR_UNHANDLED_INSN;
            return;
        }
        // invert: EQ->NE, LT->GE, LTU->BGEU
        if (funct3 & 1) T = !T;
        g_pc += T ? btype : 4;
        return;
    }

    // LB/LH/LW/LBU/LHU
    if (opcode == 0b0000011) {
        if (!callsan_can_load(rs1)) return;

        if (funct3 == 0b000) *D = sext(LOAD(S1 + itype, 1, &err), 8);
        else if (funct3 == 0b001) *D = sext(LOAD(S1 + itype, 2, &err), 16);
        else if (funct3 == 0b010) *D = LOAD(S1 + itype, 4, &err);
        else if (funct3 == 0b100) *D = LOAD(S1 + itype, 1, &err);
        else if (funct3 == 0b101) *D = LOAD(S1 + itype, 2, &err);
        else {
            g_runtime_error_type = ERROR_UNHANDLED_INSN;
            return;
        }
        if (err) {
            g_runtime_error_params[0] = S1 + itype;
            g_runtime_error_type = ERROR_LOAD;
            return;
        }
        if (!callsan_check_load(S1 + itype, 1 << (funct3 & 0b11))) {
            g_runtime_error_params[0] = S1 + itype;
            g_runtime_error_type = ERROR_CALLSAN_LOAD_STACK;
            return;
        }

        g_pc += 4;
        g_reg_written = rd;
        callsan_store(rd);
        return;
    }

    // SB/SH/SW
    if (opcode == 0b0100011) {
        if (!callsan_can_load(rs1)) return;
        if (!callsan_can_load(rs2)) return;
        if (funct3 == 0b000) STORE(S1 + stype, S2, 1, &err);
        else if (funct3 == 0b001) STORE(S1 + stype, S2, 2, &err);
        else if (funct3 == 0b010) STORE(S1 + stype, S2, 4, &err);
        else {
            g_runtime_error_params[0] = g_pc;
            g_runtime_error_type = ERROR_UNHANDLED_INSN;
            return;
        }
        if (err) {
            g_runtime_error_params[0] = S1 + stype;
            g_runtime_error_type = ERROR_STORE;
            return;
        }
        callsan_report_store(S1 + stype, 1 << funct3, rs2);
        g_pc += 4;
        return;
    }

    // non-Load I-type
    if (opcode == 0b0010011) {
        if (!callsan_can_load(rs1)) return;
        u32 shamt = itype & 31;
        if (funct3 == 0b000) *D = S1 + itype;                       // ADDI
        else if (funct3 == 0b010) *D = (i32)S1 < itype;             // SLTI
        else if (funct3 == 0b011) *D = S1 < (u32)itype;             // SLTIU
        else if (funct3 == 0b100) *D = S1 ^ itype;                  // XORI
        else if (funct3 == 0b110) *D = S1 | itype;                  // ORI
        else if (funct3 == 0b111) *D = S1 & itype;                  // ANDI
        else if (funct3 == 0b001 && funct7 == 0) *D = S1 << shamt;  // SLLI
        else if (funct3 == 0b101 && funct7 == 0) *D = S1 >> shamt;  // SRLI
        else if (funct3 == 0b101 && funct7 == 32)
            *D = (i32)S1 >> shamt;  // SRAI
        else {
            g_runtime_error_params[0] = g_pc;
            g_runtime_error_type = ERROR_UNHANDLED_INSN;
            return;
        }
        g_pc += 4;
        g_reg_written = rd;
        callsan_store(rd);
        return;
    }

    // R-type
    if (opcode == 0b0110011) {
        if (!callsan_can_load(rs1)) return;
        if (!callsan_can_load(rs2)) return;
        u32 shamt = S2 & 31;
        if (funct3 == 0b000 && funct7 == 0) *D = S1 + S2;                 // ADD
        else if (funct3 == 0b000 && funct7 == 32) *D = S1 - S2;           // SUB
        else if (funct3 == 0b001 && funct7 == 0) *D = S1 << shamt;        // SLL
        else if (funct3 == 0b010 && funct7 == 0) *D = (i32)S1 < (i32)S2;  // SLT
        else if (funct3 == 0b011 && funct7 == 0) *D = S1 < S2;      // SLTU
        else if (funct3 == 0b100 && funct7 == 0) *D = S1 ^ S2;      // XOR
        else if (funct3 == 0b101 && funct7 == 0) *D = S1 >> shamt;  // SRL
        else if (funct3 == 0b101 && funct7 == 32) *D = (i32)S1 >> shamt;  // SRA
        else if (funct3 == 0b110 && funct7 == 0) *D = S1 | S2;            // OR
        else if (funct3 == 0b111 && funct7 == 0) *D = S1 & S2;            // AND
        else if (funct3 == 0b000 && funct7 == 1) *D = (i32)S1 * (i32)S2;  // MUL
        else if (funct3 == 0b001 && funct7 == 1)
            *D = ((i64)(i32)S1 * (i64)(i32)S2) >> 32;  // MULH
        else if (funct3 == 0b010 && funct7 == 1)
            *D = ((i64)(i32)S1 * (i64)(u32)S2) >> 32;  // MULHSU
        else if (funct3 == 0b011 && funct7 == 1)
            *D = ((u64)S1 * (u64)S2) >> 32;                            // MULHU
        else if (funct3 == 0b100 && funct7 == 1) *D = div32(S1, S2);   // DIV
        else if (funct3 == 0b101 && funct7 == 1) *D = divu32(S1, S2);  // DIVU
        else if (funct3 == 0b110 && funct7 == 1) *D = rem32(S1, S2);   // REM
        else if (funct3 == 0b111 && funct7 == 1) *D = remu32(S1, S2);  // REMU
        else {
            g_runtime_error_params[0] = g_pc;
            g_runtime_error_type = ERROR_UNHANDLED_INSN;
            return;
        }
        g_pc += 4;
        g_reg_written = rd;
        callsan_store(rd);
        return;
    }
    // SYSTEM instructions
    if (opcode == 0x73) {
        if (funct3 == 0b000 && itype == 0) {
            do_syscall();
            return;
        }
        if (funct3 == 0b000 && itype == 1) {
            do_ebreak();
            return;
        }

        // from here on, all others require supervisor mode
        // slight imprecision for CSRs in general,
        // but we only support privileged CSRs

        if (g_privilege_level == PRIV_USER) {
            g_runtime_error_params[0] = g_pc;
            g_runtime_error_type = ERROR_PROTECTION;
            return;
        }

        if (funct3 == 0b000) {
            if (itype == 0x102) {
                do_sret();
                return;
            }
        } else {                    // all CSR ops
            if (funct3 == 0b001) {  // CSRRW
                u32 old = rdcsr(itype);
                wrcsr(itype, g_regs[rs1]);
                g_regs[rd] = old;
            } else if (funct3 == 0b010) {  // CSRRS
                u32 old = rdcsr(itype);
                if (rs1 != 0) wrcsr(itype, old | g_regs[rs1]);
                g_regs[rd] = old;
            } else if (funct3 == 0b011) {  // CSRRC
                u32 old = rdcsr(itype);
                if (rs1 != 0) wrcsr(itype, old & ~g_regs[rs1]);
                g_regs[rd] = old;
            } else if (funct3 == 0b101) {  // CSRRWI
                g_regs[rd] = rdcsr(itype);
                wrcsr(itype, rs1);         // used as imm
            } else if (funct3 == 0b110) {  // CSRRSI
                u32 old = rdcsr(itype);
                if (rs1 != 0) wrcsr(itype, old | rs1);
                g_regs[rd] = old;
            } else if (funct3 == 0b111) {  // CSRRCI
                u32 old = rdcsr(itype);
                if (rs1 != 0) wrcsr(itype, old & ~rs1);
                g_regs[rd] = old;
            } else {
                goto end;
            }
            g_reg_written = rd;
            callsan_store(rd);
        }
        g_pc += 4;
        return;
    }

    // if i reached here, it's an unhandled instruction
end:
    g_runtime_error_params[0] = g_pc;
    g_runtime_error_type = ERROR_UNHANDLED_INSN;
    return;
}

static size_t u32_to_str(u32 val, char buf[11]) {
    char tmp[10];
    size_t i = 0;
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    while (val) {
        tmp[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    for (size_t j = 0; j < i; ++j) buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
    return i;
}

static size_t i32_to_str(i32 val, char buf[12]) {
    if (val < 0) {
        buf[0] = '-';
        return 1 + u32_to_str((u32)(-(i64)val), buf + 1);
    }
    return u32_to_str((u32)val, buf);
}

static size_t u32_to_str_hex(u32 val, char buf[11]) {
    const char *digits = "0123456789abcdef";
    char tmp[8];
    size_t i = 0;
    buf[0] = '0';
    buf[1] = 'x';
    if (val == 0) {
        buf[2] = '0';
        buf[3] = '\0';
        return 3;
    }
    while (val) {
        tmp[i++] = digits[val & 0xF];
        val >>= 4;
    }
    for (size_t j = 0; j < i; ++j) buf[2 + j] = tmp[i - 1 - j];
    buf[2 + i] = '\0';
    return 2 + i;
}

size_t disassemble(u32 inst, char *buf, size_t buflen) {
    if (buflen == 0) return 0;
    buf[0] = '\0';
    size_t pos = 0;

    u32 rd = extr(inst, 11, 7);
    u32 rs1 = extr(inst, 19, 15);
    u32 rs2 = extr(inst, 24, 20);
    u32 funct7 = extr(inst, 31, 25);
    u32 funct3 = extr(inst, 14, 12);

    i32 btype = sext((extr(inst, 31, 31) << 12) | (extr(inst, 7, 7) << 11) |
                         (extr(inst, 30, 25) << 5) | (extr(inst, 11, 8) << 1),
                     13);
    i32 stype = sext((extr(inst, 31, 25) << 5) | (extr(inst, 11, 7)), 12);
    i32 jtype = sext((extr(inst, 31, 31) << 20) | (extr(inst, 19, 12) << 12) |
                         (extr(inst, 20, 20) << 11) | (extr(inst, 30, 21) << 1),
                     21);
    i32 itype = sext(extr(inst, 31, 20), 12);
    u32 utype = extr(inst, 31, 12) << 12;

    u32 opcode = extr(inst, 6, 0);

#define APPEND_STR(s)                                       \
    do {                                                    \
        const char *_p = s;                                 \
        while (*_p && pos + 1 < buflen) buf[pos++] = *_p++; \
    } while (0)

#define APPEND_U32(x)                                          \
    do {                                                       \
        char tmp[11];                                          \
        u32_to_str(x, tmp);                                    \
        for (size_t _i = 0; tmp[_i] && pos + 1 < buflen; _i++) \
            buf[pos++] = tmp[_i];                              \
    } while (0)

#define APPEND_U32_HEX(x)                                      \
    do {                                                       \
        char tmp[11];                                          \
        u32_to_str_hex(x, tmp);                                \
        for (size_t _i = 0; tmp[_i] && pos + 1 < buflen; _i++) \
            buf[pos++] = tmp[_i];                              \
    } while (0)

#define APPEND_I32(x)                                          \
    do {                                                       \
        char tmp[12];                                          \
        i32_to_str(x, tmp);                                    \
        for (size_t _i = 0; tmp[_i] && pos + 1 < buflen; _i++) \
            buf[pos++] = tmp[_i];                              \
    } while (0)

#define APPEND_REG(r)    \
    do {                 \
        APPEND_STR("x"); \
        APPEND_U32(r);   \
    } while (0)

    if ((inst & 0b11) != 0b11) {
        u16 cinst = (u16)inst;
        u32 copcode = extr(cinst, 1, 0);
        u32 cfunct3 = extr(cinst, 15, 13);

        if (copcode == 0b00) {
            if (cfunct3 == 0b000) {
                if (c_addi4spn_nzuimm(cinst) == 0) {
                    APPEND_STR("<c.addi4spn with imm 0: reserved>");
                    goto done;
                }
                APPEND_STR("c.addi4spn ");
                APPEND_REG(c_reg(extr(cinst, 4, 2)));
                APPEND_STR(", ");
                APPEND_U32(c_addi4spn_nzuimm(cinst));
                goto done;
            }
            if (cfunct3 == 0b010) {
                APPEND_STR("c.lw ");
                APPEND_REG(c_reg(extr(cinst, 4, 2)));
                APPEND_STR(", ");
                APPEND_U32(c_lw_sw_off(cinst));
                APPEND_STR("(");
                APPEND_REG(c_reg(extr(cinst, 9, 7)));
                APPEND_STR(")");
                goto done;
            }
            if (cfunct3 == 0b110) {
                APPEND_STR("c.sw ");
                APPEND_REG(c_reg(extr(cinst, 4, 2)));
                APPEND_STR(", ");
                APPEND_U32(c_lw_sw_off(cinst));
                APPEND_STR("(");
                APPEND_REG(c_reg(extr(cinst, 9, 7)));
                APPEND_STR(")");
                goto done;
            }
        } else if (copcode == 0b01) {
            if (cfunct3 == 0b000) {
                u32 crd = extr(cinst, 11, 7);
                i32 imm = c_imm6(cinst);
                if (crd == 0 && imm == 0) {
                    APPEND_STR("c.nop");
                    goto done;
                }
                if (crd == 0) {
                    APPEND_STR("c.nop ");
                    APPEND_U32_HEX(imm);
                    goto done;
                }
                if (imm == 0) {
                    APPEND_STR("<hint>");
                    goto done;
                }
                APPEND_STR("c.addi ");
                APPEND_REG(crd);
                APPEND_STR(", ");
                APPEND_I32(imm);
                goto done;
            }
            if (cfunct3 == 0b001) {
                APPEND_STR("c.jal ");
                APPEND_I32(c_jump_off(cinst));
                goto done;
            }
            if (cfunct3 == 0b010) {
                if (extr(cinst, 11, 7) == 0) {
                    APPEND_STR("<hint>");
                    goto done;
                }
                APPEND_STR("c.li ");
                APPEND_REG(extr(cinst, 11, 7));
                APPEND_STR(", ");
                APPEND_I32(c_imm6(cinst));
                goto done;
            }
            if (cfunct3 == 0b011) {
                u32 crd = extr(cinst, 11, 7);
                if (crd == 0) {
                    APPEND_STR("<hint>");
                    goto done;
                }
                if (crd == REG_SP) {
                    if (c_addi16sp_nzimm(cinst) == 0) {
                        APPEND_STR("<c.addi16sp with imm 0: reserved>");
                        goto done;
                    }
                    APPEND_STR("c.addi16sp sp, ");
                    APPEND_I32(c_addi16sp_nzimm(cinst));
                } else {
                    if (c_imm6(cinst) == 0) {
                        APPEND_STR("<c.lui with imm 0: reserved>");
                        goto done;
                    }
                    APPEND_STR("c.lui ");
                    APPEND_REG(crd);
                    APPEND_STR(", ");
                    APPEND_I32(c_imm6(cinst));
                }
                goto done;
            }
            if (cfunct3 == 0b100) {
                u32 funct2 = extr(cinst, 11, 10);
                u32 crd = c_reg(extr(cinst, 9, 7));

                if (funct2 == 0b00 || funct2 == 0b01) {
                    if (extr(cinst, 6, 2) == 0) {
                        APPEND_STR("<right shift with imm 0: hint>");
                        goto done;
                    }
                    if (extr(cinst, 12, 12) == 1) {
                        APPEND_STR("<right shift with imm >= 32: reserved>");
                        goto done;
                    }

                    APPEND_STR(funct2 == 0b00 ? "c.srli " : "c.srai ");
                    APPEND_REG(crd);
                    APPEND_STR(", ");
                    APPEND_U32(extr(cinst, 6, 2));

                    goto done;
                }
                if (funct2 == 0b10) {
                    APPEND_STR("c.andi ");
                    APPEND_REG(crd);
                    APPEND_STR(", ");
                    APPEND_I32(c_imm6(cinst));
                    goto done;
                }
                if (funct2 == 0b11) {
                    // TODO: clean this up
                    if (extr(cinst, 12, 12) == 1) {
                        APPEND_STR("<unsupported>");  // c.sub vs c.subw
                        goto done;
                    }
                    u32 op = extr(cinst, 6, 5);
                    if (op == 0b00) APPEND_STR("c.sub ");
                    else if (op == 0b01) APPEND_STR("c.xor ");
                    else if (op == 0b10) APPEND_STR("c.or ");
                    else APPEND_STR("c.and ");
                    APPEND_REG(crd);
                    APPEND_STR(", ");
                    APPEND_REG(c_reg(extr(cinst, 4, 2)));
                    goto done;
                }
            }
            if (cfunct3 == 0b101) {
                APPEND_STR("c.j ");
                APPEND_I32(c_jump_off(cinst));
                goto done;
            }
            if (cfunct3 == 0b110 || cfunct3 == 0b111) {
                APPEND_STR(cfunct3 == 0b110 ? "c.beqz " : "c.bnez ");
                APPEND_REG(c_reg(extr(cinst, 9, 7)));
                APPEND_STR(", ");
                APPEND_I32(c_branch_off(cinst));
                goto done;
            }
        } else if (copcode == 0b10) {
            if (cfunct3 == 0b000) {
                if (extr(cinst, 11, 7) == 0) {
                    APPEND_STR("<c.slli with reg 0: hint>");
                    goto done;
                }
                if (extr(cinst, 6, 2) == 0) {
                    APPEND_STR("<c.slli with imm 0: hint>");
                    goto done;
                }
                if (extr(cinst, 12, 12) == 1) {
                    APPEND_STR("<c.slli with imm >= 32: reserved>");
                    goto done;
                }
                APPEND_STR("c.slli ");
                APPEND_REG(extr(cinst, 11, 7));
                APPEND_STR(", ");
                APPEND_U32(extr(cinst, 6, 2));
                goto done;
            }
            if (cfunct3 == 0b010) {
                if (extr(cinst, 11, 7) == 0) {
                    APPEND_STR("<c.lwsp with rd 0: reserved>");
                    goto done;
                }
                APPEND_STR("c.lwsp ");
                APPEND_REG(extr(cinst, 11, 7));
                APPEND_STR(", ");
                APPEND_U32(c_lwsp_off(cinst));
                goto done;
            }
            if (cfunct3 == 0b100) {
                u32 crd = extr(cinst, 11, 7);
                u32 crs2 = extr(cinst, 6, 2);
                if (extr(cinst, 12, 12) == 0) {
                    if (crs2 == 0) {
                        if (crd == 0) {
                            APPEND_STR("<c.jr x0: reserved>");
                            goto done;
                        }
                        APPEND_STR("c.jr ");
                        APPEND_REG(crd);
                    } else {
                        if (crd == 0) {
                            APPEND_STR("<c.mv x0: hint>");
                            goto done;
                        }
                        APPEND_STR("c.mv ");
                        APPEND_REG(crd);
                        APPEND_STR(", ");
                        APPEND_REG(crs2);
                    }
                    goto done;
                }
                if (crs2 == 0) {
                    if (crd == 0) {
                        APPEND_STR("c.ebreak");
                    } else {
                        APPEND_STR("c.jalr ");
                        APPEND_REG(crd);
                    }
                    goto done;
                }
                if (crd == 0) {
                    APPEND_STR("<c.add x0: hint>");
                    goto done;
                }
                APPEND_STR("c.add ");
                APPEND_REG(crd);
                APPEND_STR(", ");
                APPEND_REG(crs2);
                goto done;
            }
            if (cfunct3 == 0b110) {
                APPEND_STR("c.swsp ");
                APPEND_REG(extr(cinst, 6, 2));
                APPEND_STR(", ");
                APPEND_U32(c_swsp_off(cinst));
                goto done;
            }
        }

        APPEND_STR("<unhandled compressed>");
        goto done;
    }

    // LUI
    if (opcode == 0b0110111) {
        // it has been requested to be compatible with RARS here
        // and RARS does the following:
        // li a0, 0xaabbccdd becomes
        // lui x10, 0xfffaabbd
        // addi x10, 0xfffffcdd
        APPEND_STR("lui x");
        APPEND_U32(rd);
        APPEND_STR(", ");
        APPEND_U32_HEX((u32)((i32)utype >> 12));
        goto done;
    }

    // AUIPC
    if (opcode == 0b0010111) {
        APPEND_STR("auipc x");
        APPEND_U32(rd);
        APPEND_STR(", ");
        APPEND_U32_HEX((u32)((i32)utype >> 12));
        goto done;
    }

    // JAL
    if (opcode == 0b1101111) {
        APPEND_STR("jal x");
        APPEND_U32(rd);
        APPEND_STR(", ");
        APPEND_I32(jtype);
        goto done;
    }

    // JALR
    if (opcode == 0b1100111) {
        if (funct3 != 0b000) {
            APPEND_STR("<invalid jalr funct3>");
            goto done;
        }
        APPEND_STR("jalr x");
        APPEND_U32(rd);
        APPEND_STR(", x");
        APPEND_U32(rs1);
        APPEND_STR(", ");
        APPEND_I32(itype);
        goto done;
    }

    // Branch
    if (opcode == 0b1100011) {
        const char *name;
        if (funct3 == 0b000) name = "beq";
        else if (funct3 == 0b001) name = "bne";
        else if (funct3 == 0b100) name = "blt";
        else if (funct3 == 0b101) name = "bge";
        else if (funct3 == 0b110) name = "bltu";
        else if (funct3 == 0b111) name = "bgeu";
        else {
            APPEND_STR("<invalid branch funct3>");
            goto done;
        }
        APPEND_STR(name);
        APPEND_STR(" x");
        APPEND_U32(rs1);
        APPEND_STR(", x");
        APPEND_U32(rs2);
        APPEND_STR(", ");
        APPEND_I32(btype);
        goto done;
    }

    // Load
    if (opcode == 0b0000011) {
        const char *name;
        if (funct3 == 0b000) name = "lb";
        else if (funct3 == 0b001) name = "lh";
        else if (funct3 == 0b010) name = "lw";
        else if (funct3 == 0b100) name = "lbu";
        else if (funct3 == 0b101) name = "lhu";
        else {
            APPEND_STR("<invalid load funct3>");
            goto done;
        }
        APPEND_STR(name);
        APPEND_STR(" x");
        APPEND_U32(rd);
        APPEND_STR(", ");
        APPEND_I32(itype);
        APPEND_STR("(x");
        APPEND_U32(rs1);
        APPEND_STR(")");
        goto done;
    }

    // Store
    if (opcode == 0b0100011) {
        const char *name;
        if (funct3 == 0b000) name = "sb";
        else if (funct3 == 0b001) name = "sh";
        else if (funct3 == 0b010) name = "sw";
        else {
            APPEND_STR("<invalid store funct3>");
            goto done;
        }
        APPEND_STR(name);
        APPEND_STR(" x");
        APPEND_U32(rs2);
        APPEND_STR(", ");
        APPEND_I32(stype);
        APPEND_STR("(x");
        APPEND_U32(rs1);
        APPEND_STR(")");
        goto done;
    }

    // I-type arithmetic
    // printing is very ugly, but it is made to match RARS
    // especially in LUI+ADDI handling
    if (opcode == 0b0010011) {
        bool shift = false;
        const char *name;
        if (funct3 == 0b000) name = "addi";
        else if (funct3 == 0b010) name = "slti";
        else if (funct3 == 0b011) name = "sltiu";
        else if (funct3 == 0b100) name = "xori";
        else if (funct3 == 0b110) name = "ori";
        else if (funct3 == 0b111) name = "andi";
        else if (funct3 == 0b001 && funct7 == 0) shift = true, name = "slli";
        else if (funct3 == 0b101 && funct7 == 0) shift = true, name = "srli";
        else if (funct3 == 0b101 && funct7 == 32) shift = true, name = "srai";
        else {
            APPEND_STR("<invalid I-type funct3>");
            goto done;
        }
        APPEND_STR(name);
        APPEND_STR(" x");
        APPEND_U32(rd);
        APPEND_STR(", x");
        APPEND_U32(rs1);
        APPEND_STR(", ");
        if (shift) APPEND_U32(itype & 31);
        else
            APPEND_U32_HEX(
                (u32)itype);  // compliant with RARS disassembly in hex mode
        goto done;
    }

    // R-type
    if (opcode == 0b0110011) {
        const char *name;
        if (funct3 == 0b000 && funct7 == 0) name = "add";
        else if (funct3 == 0b000 && funct7 == 32) name = "sub";
        else if (funct3 == 0b001 && funct7 == 0) name = "sll";
        else if (funct3 == 0b010 && funct7 == 0) name = "slt";
        else if (funct3 == 0b011 && funct7 == 0) name = "sltu";
        else if (funct3 == 0b100 && funct7 == 0) name = "xor";
        else if (funct3 == 0b101 && funct7 == 0) name = "srl";
        else if (funct3 == 0b101 && funct7 == 32) name = "sra";
        else if (funct3 == 0b110 && funct7 == 0) name = "or";
        else if (funct3 == 0b111 && funct7 == 0) name = "and";
        else if (funct3 == 0b000 && funct7 == 1) name = "mul";
        else if (funct3 == 0b001 && funct7 == 1) name = "mulh";
        else if (funct3 == 0b010 && funct7 == 1) name = "mulhsu";
        else if (funct3 == 0b011 && funct7 == 1) name = "mulhu";
        else if (funct3 == 0b100 && funct7 == 1) name = "div";
        else if (funct3 == 0b101 && funct7 == 1) name = "divu";
        else if (funct3 == 0b110 && funct7 == 1) name = "rem";
        else if (funct3 == 0b111 && funct7 == 1) name = "remu";
        else {
            APPEND_STR("<invalid R-type funct3>");
            goto done;
        }
        APPEND_STR(name);
        APPEND_STR(" x");
        APPEND_U32(rd);
        APPEND_STR(", x");
        APPEND_U32(rs1);
        APPEND_STR(", x");
        APPEND_U32(rs2);
        goto done;
    }

    // SYSTEM
    if (opcode == 0x73) {
        u32 csr = extr(inst, 31, 20);  // CSR address is in the immediate field

        if (funct3 == 0b000) {
            const char *name;
            if (inst == 0x10200073) name = "sret";
            else if (inst == 0x00000073) name = "ecall";
            else if (inst == 0x00100073) name = "ebreak";
            else {
                APPEND_STR("<unhandled system instruction>");
                goto done;
            }
            APPEND_STR(name);
        } else if (funct3 == 0b001) {
            // csrrw rd, csr, rs1
            APPEND_STR("csrrw x");
            APPEND_U32(rd);
            APPEND_STR(", ");
            APPEND_U32_HEX(csr);
            APPEND_STR(", x");
            APPEND_U32(rs1);
        } else if (funct3 == 0b010) {
            // csrrs rd, csr, rs1
            APPEND_STR("csrrs x");
            APPEND_U32(rd);
            APPEND_STR(", ");
            APPEND_U32_HEX(csr);
            APPEND_STR(", x");
            APPEND_U32(rs1);
        } else if (funct3 == 0b011) {
            // csrrc rd, csr, rs1
            APPEND_STR("csrrc x");
            APPEND_U32(rd);
            APPEND_STR(", ");
            APPEND_U32_HEX(csr);
            APPEND_STR(", x");
            APPEND_U32(rs1);
        } else if (funct3 == 0b101) {
            // csrrwi rd, csr, uimm
            APPEND_STR("csrrwi x");
            APPEND_U32(rd);
            APPEND_STR(", ");
            APPEND_U32_HEX(csr);
            APPEND_STR(", ");
            APPEND_U32(rs1);  // rs1 field used as 5-bit unsigned immediate
        } else if (funct3 == 0b110) {
            // csrrsi rd, csr, uimm
            APPEND_STR("csrrsi x");
            APPEND_U32(rd);
            APPEND_STR(", ");
            APPEND_U32_HEX(csr);
            APPEND_STR(", ");
            APPEND_U32(rs1);
        } else if (funct3 == 0b111) {
            // csrrci rd, csr, uimm
            APPEND_STR("csrrci x");
            APPEND_U32(rd);
            APPEND_STR(", ");
            APPEND_U32_HEX(csr);
            APPEND_STR(", ");
            APPEND_U32(rs1);
        } else {
            APPEND_STR("<invalid system funct3>");
        }
        goto done;
    }
    // default unknown
    APPEND_STR("<unhandled>");

done:
    if (pos < buflen) buf[pos] = '\0';
    else buf[buflen - 1] = '\0';
    return pos;
}

// wrapper for the webui
u32 emu_load(u32 addr, int size) {
    bool err;
    u32 val = LOAD(addr, size, &err);
    if (err) return 0;
    return val;
}

char g_emu_disassemble_buf[64];

size_t emu_disassemble(u32 inst) {
    return disassemble(inst, g_emu_disassemble_buf, 64);
}

void emulator_enter_kernel(void) { g_privilege_level = PRIV_SUPERVISOR; }

void emulator_leave_kernel(void) { g_privilege_level = PRIV_USER; }

void emulator_interrupt_set_pending(u32 intno) {
    g_csr[CSR_MIP] |= 1u << intno;
}

void emulator_interrupt_clear_pending(u32 intno) {
    g_csr[CSR_MIP] &= ~(1u << intno);
}

void emulator_deliver_interrupt(u32 cause) {
    bool is_interrupt = cause & CAUSE_INTERRUPT;
    u32 off = cause & ~CAUSE_INTERRUPT;
    assert(off < 32);

    int prev_privilege = g_privilege_level;

    g_csr[CSR_SEPC] = g_pc;
    g_csr[CSR_SCAUSE] = cause;

    u32 status = g_csr[CSR_MSTATUS];
    bool was_enabled = status & STATUS_SIE;
    g_privilege_level = PRIV_SUPERVISOR;

    // STATUS.xIE = 0
    status &= ~STATUS_SIE;
    // STATUS.xPIE = STATUS.xIE of the old privilege
    status = (status & ~STATUS_SPIE) | (was_enabled ? STATUS_SPIE : 0);
    // STATUS.xPP = prev_privilege
    // NOTE: SPP is 1 bit long
    status = (status & ~STATUS_SPP) |
             ((prev_privilege != PRIV_USER) ? STATUS_SPP : 0);
    g_csr[CSR_MSTATUS] = status;

    u32 tvec_base = g_csr[CSR_STVEC] & ~0x3u;
    u32 tvec_mode = g_csr[CSR_STVEC] & 0x3u;
    if (tvec_mode == 1 && is_interrupt) g_pc = tvec_base + (off << 2);
    else g_pc = tvec_base;
}

void emulator_init(void) {
    g_exited = false;
    g_exit_code = 0;

    memset(g_regs, 0, sizeof(g_regs));
    g_pc = TEXT_BASE;
    g_mem_written_len = 0;
    g_mem_written_addr = 0;
    g_reg_written = 0;
    g_error_line = 0;
    g_error = NULL;

    memset(g_runtime_error_params, 0, sizeof(g_runtime_error_params));
    g_runtime_error_type = 0;

    prepare_aux_sections();

    memset(g_csr, 0, sizeof(g_csr));
    g_csr[CSR_MSTATUS] |= STATUS_SIE;
    g_csr[CSR_MIE] |= 1u << (CAUSE_SUPERVISOR_SOFTWARE & ~CAUSE_INTERRUPT);
    g_csr[CSR_MIE] |= 1u << (CAUSE_SUPERVISOR_TIMER & ~CAUSE_INTERRUPT);
    g_csr[CSR_MIE] |= 1u << (CAUSE_SUPERVISOR_EXTERNAL & ~CAUSE_INTERRUPT);
}

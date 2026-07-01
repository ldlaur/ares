#include "ares/core.h"

#include <stddef.h>

#include "ares/callsan.h"
#include "ares/dev.h"
#include "ares/elf.h"
#include "ares/emulate.h"

export Section *g_text, *g_data, *g_stack, *g_kernel_text, *g_kernel_data,
    *g_mmio;

typedef struct PcrelHiReloc {
    u32 label_addr;
    u32 dest_addr;
} PcrelHiReloc;
ARES_ARRAY_TYPE(PcrelHiReloc);

ARES_ARRAY(SectionPtr) g_sections = ARES_ARRAY_NEW(SectionPtr);
ARES_ARRAY(Extern) g_externs = ARES_ARRAY_NEW(Extern);
ARES_ARRAY(LabelData) g_labels = ARES_ARRAY_NEW(LabelData);
ARES_ARRAY(Global) g_globals = ARES_ARRAY_NEW(Global);
ARES_ARRAY(LocalLabel) g_local_labels = ARES_ARRAY_NEW(LocalLabel);
ARES_ARRAY(PcrelHiReloc) g_pcrel_hi_relocs = ARES_ARRAY_NEW(PcrelHiReloc);

static ARES_ARRAY(DeferredInsn) g_deferred_insn = ARES_ARRAY_NEW(DeferredInsn);

static Section *g_section;

export bool g_in_fixup;
export u32 g_error_line;
export const char *g_error;

export u32 g_runtime_error_params[2];
export Error g_runtime_error_type;

static bool g_allow_externs;

// NOTE: this may seem like it can be static, but it's used elsewhere (like in
// cli.c)
const char *const REGISTER_NAMES[] = {
    "zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "fp", "s1", "a0",
    "a1",   "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
    "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};

const char *const CSR_NAMES[] = {
    [0x100] = "sstatus",  [0x104] = "sie",     [0x105] = "stvec",
    [0x140] = "sscratch", [0x141] = "sepc",    [0x142] = "scause",
    [0x144] = "sip",      [0x300] = "mstatus", [0x302] = "medeleg",
    [0x303] = "mideleg",  [0x304] = "mie",     [0x305] = "mtvec",
    [0x340] = "mscratch", [0x341] = "mepc",    [0x342] = "mcause",
    [0x344] = "mip"};

// clang-format off
u32 DS1S2(u32 d, u32 s1, u32 s2) { return (d << 7) | (s1 << 15) | (s2 << 20); }
#define InstA(Name, op2, op12, one, mul) u32 Name(u32 d, u32 s1, u32 s2)  { return 0b11 | (op2 << 2) | (op12 << 12) | DS1S2(d, s1, s2) | ((one*0b01000) << 27) | (mul << 25); }
#define InstI(Name, op2, op12) u32 Name(u32 d, u32 s1, u32 imm) { return 0b11 | (op2 << 2) | ((imm & 0xfff) << 20) | (s1 << 15) | (op12 << 12) | (d << 7); }

InstI(ADDI,  0b00100, 0b000)
InstI(SLTI,  0b00100, 0b010)
InstI(SLTIU, 0b00100, 0b011)
InstI(XORI,  0b00100, 0b100)
InstI(ORI,   0b00100, 0b110)
InstI(ANDI,  0b00100, 0b111)
InstI(CSRRW, 0x1C, 0b001)
InstI(CSRRS, 0x1C, 0b010)
InstI(CSRRC, 0x1C, 0b011)
InstI(CSRRWI, 0x1C, 0b101)
InstI(CSRRSI, 0x1C, 0b110)
InstI(CSRRCI, 0x1C, 0b111)

InstA(SLLI,  0b00100, 0b001, 0, 0)
InstA(SRLI,  0b00100, 0b101, 0, 0)
InstA(SRAI,  0b00100, 0b101, 1, 0)
InstA(ADD,   0b01100, 0b000, 0, 0)
InstA(SUB,   0b01100, 0b000, 1, 0)
InstA(MUL,   0b01100, 0b000, 0, 1)
InstA(SLL,   0b01100, 0b001, 0, 0)
InstA(MULH,  0b01100, 0b001, 0, 1)
InstA(SLT,   0b01100, 0b010, 0, 0)
InstA(MULHSU,0b01100, 0b010, 0, 1)
InstA(SLTU,  0b01100, 0b011, 0, 0)
InstA(MULHU, 0b01100, 0b011, 0, 1)
InstA(XOR,   0b01100, 0b100, 0, 0)
InstA(DIV,   0b01100, 0b100, 0, 1)
InstA(SRL,   0b01100, 0b101, 0, 0)
InstA(SRA,   0b01100, 0b101, 1, 0)
InstA(DIVU,  0b01100, 0b101, 0, 1)
InstA(OR,    0b01100, 0b110, 0, 0)
InstA(REM,   0b01100, 0b110, 0, 1)
InstA(AND,   0b01100, 0b111, 0, 0)
InstA(REMU,  0b01100, 0b111, 0, 1)

u32 Store(u32 src, u32 base, u32 off, u32 width) { return 0b0100011 | ((off & 31) << 7) | (width << 12) | (base << 15) | (src << 20) | ((off >> 5) << 25); }
u32 Load(u32 rd, u32 rs, u32 off, u32 width) { return 0b0000011 | (rd << 7) | (width << 12) | (rs << 15) | (off << 20); }
u32 LB(u32 rd, u32 rs, u32 off) { return Load(rd, rs, off, 0); }
u32 LH(u32 rd, u32 rs, u32 off) { return Load(rd, rs, off, 1); }
u32 LW(u32 rd, u32 rs, u32 off) { return Load(rd, rs, off, 2); }
u32 LBU(u32 rd, u32 rs, u32 off) { return Load(rd, rs, off, 4); }
u32 LHU(u32 rd, u32 rs, u32 off) { return Load(rd, rs, off, 5); }
u32 SB(u32 src, u32 base, u32 off) { return Store(src, base, off, 0); }
u32 SH(u32 src, u32 base, u32 off) { return Store(src, base, off, 1); }
u32 SW(u32 src, u32 base, u32 off) { return Store(src, base, off, 2); }
u32 Branch(u32 rs1, u32 rs2, u32 off, u32 func) { return 0b1100011 | (((off >> 11) & 1) << 7) | (((off >> 1) & 15) << 8) | (func << 12) | (rs1 << 15) | (rs2 << 20) | (((off >> 5) & 63) << 25) | (((off >> 12) & 1) << 31); }
u32 BEQ(u32 rs1, u32 rs2, u32 off)  { return Branch(rs1, rs2, off, 0); }
u32 BNE(u32 rs1, u32 rs2, u32 off)  { return Branch(rs1, rs2, off, 1); }
u32 BLT(u32 rs1, u32 rs2, u32 off)  { return Branch(rs1, rs2, off, 4); }
u32 BGE(u32 rs1, u32 rs2, u32 off)  { return Branch(rs1, rs2, off, 5); }
u32 BLTU(u32 rs1, u32 rs2, u32 off) { return Branch(rs1, rs2, off, 6); }
u32 BGEU(u32 rs1, u32 rs2, u32 off) { return Branch(rs1, rs2, off, 7); }
u32 LUI(u32 rd, u32 off) { return 0b0110111 | (rd << 7) | (off << 12); }
u32 AUIPC(u32 rd, u32 off) { return 0b0010111 | (rd << 7) | (off << 12); }
u32 JAL(u32 rd, u32 off) { return 0b1101111 | (rd << 7) | (((off >> 12) & 255) << 12) | (((off >> 11) & 1) << 20) | (((off >> 1) & 1023) << 21) | ((off >> 20) << 31); }
u32 JALR(u32 rd, u32 rs1, u32 off) { return 0b1100111 | (rd << 7) | (rs1 << 15) | (off << 20); }

// clang-format on

// compressed instruction
static inline u16 cbits(u32 val, u32 hi, u32 lo, u32 dst) {
    return (u16)(extr(val, hi, lo) << dst);
}

static inline u16 cfunct3(u32 funct3) { return (u16)(funct3 << 13); }

u16 C_LWSP(Reg rd, u32 off) {
    return cfunct3(0b010) | cbits(off, 5, 5, 12) | cbits(rd, 4, 0, 7) |
           cbits(off, 4, 2, 4) | cbits(off, 7, 6, 2) | 0b10;
}

u16 C_SWSP(Reg rs2, u32 off) {
    return cfunct3(0b110) | cbits(off, 5, 2, 9) | cbits(off, 7, 6, 7) |
           cbits(rs2, 4, 0, 2) | 0b10;
}

u16 C_LW(SmallReg rd_, SmallReg rs1_, u32 off) {
    return cfunct3(0b010) | cbits(off, 5, 3, 10) | (cbits(rs1_, 2, 0, 7)) |
           cbits(off, 2, 2, 6) | cbits(off, 6, 6, 5) | (cbits(rd_, 2, 0, 2)) |
           0b00;
}

u16 C_SW(SmallReg rs2_, SmallReg rs1_, u32 off) {
    return cfunct3(0b110) | cbits(off, 5, 3, 10) | cbits(rs1_, 2, 0, 7) |
           cbits(off, 2, 2, 6) | cbits(off, 6, 6, 5) | cbits(rs2_, 4, 0, 2) |
           0b00;
}

static u16 C_J_KIND(u32 funct3, u32 off) {
    return cfunct3(funct3) | cbits(off, 11, 11, 12) | cbits(off, 4, 4, 11) |
           cbits(off, 9, 8, 9) | cbits(off, 10, 10, 8) | cbits(off, 6, 6, 7) |
           cbits(off, 7, 7, 6) | cbits(off, 3, 1, 3) | cbits(off, 5, 5, 2) |
           0b01;
}

u16 C_J(u32 off) { return C_J_KIND(0b101, off); }

u16 C_JAL(u32 off) { return C_J_KIND(0b001, off); }

u16 C_JR(Reg rs1) { return (0b1000 << 12) | (u16)((rs1 & 0x1f) << 7) | 0b10; }

u16 C_JALR(Reg rs1) { return (0b1001 << 12) | (u16)((rs1 & 0x1f) << 7) | 0b10; }

static u16 C_BRANCH_KIND(u32 funct3, SmallReg rs1_, u32 off) {
    return cfunct3(funct3) | cbits(off, 8, 8, 12) | cbits(off, 4, 3, 10) |
           cbits(rs1_, 2, 0, 7) | cbits(off, 7, 6, 5) | cbits(off, 2, 1, 3) |
           cbits(off, 5, 5, 2) | 0b01;
}

u16 C_BEQZ(SmallReg rs1_, u32 off) { return C_BRANCH_KIND(0b110, rs1_, off); }

u16 C_BNEZ(SmallReg rs1_, u32 off) { return C_BRANCH_KIND(0b111, rs1_, off); }

u16 C_LI(Reg rd, u32 imm) {
    return cfunct3(0b010) | cbits(imm, 5, 5, 12) | (u16)((rd & 0x1f) << 7) |
           cbits(imm, 4, 0, 2) | 0b01;
}

u16 C_LUI(Reg rd, u32 nzimm) {
    return cfunct3(0b011) | cbits(nzimm, 5, 5, 12) | (u16)((rd & 0x1f) << 7) |
           cbits(nzimm, 4, 0, 2) | 0b01;
}

u16 C_ADDI(Reg rd, u32 nzimm) {
    return cfunct3(0b000) | cbits(nzimm, 5, 5, 12) | (u16)((rd & 0x1f) << 7) |
           cbits(nzimm, 4, 0, 2) | 0b01;
}

u16 C_ADDI16SP(u32 nzimm) {
    return cfunct3(0b011) | cbits(nzimm, 9, 9, 12) | (2 << 7) |
           cbits(nzimm, 4, 4, 6) | cbits(nzimm, 6, 6, 5) |
           cbits(nzimm, 8, 7, 3) | cbits(nzimm, 5, 5, 2) | 0b01;
}

u16 C_ADDI4SPN(SmallReg rd_, u32 nzuimm) {
    return cfunct3(0b000) | cbits(nzuimm, 5, 4, 11) | cbits(nzuimm, 9, 6, 7) |
           cbits(nzuimm, 2, 2, 6) | cbits(nzuimm, 3, 3, 5) |
           cbits(rd_, 2, 0, 2) | 0b00;
}

u16 C_SLLI(Reg rd, u32 shamt) {
    return cfunct3(0b000) | cbits(shamt, 5, 5, 12) | cbits(rd, 4, 0, 7) |
           cbits(shamt, 4, 0, 2) | 0b10;
}

static u16 C_SHIFT_RIGHT(u32 funct2, SmallReg rd_, u32 shamt) {
    return cfunct3(0b100) | cbits(shamt, 5, 5, 12) | (u16)(funct2 << 10) |
           cbits(rd_, 2, 0, 7) | cbits(shamt, 4, 0, 2) | 0b01;
}

u16 C_SRLI(SmallReg rd_, u32 shamt) { return C_SHIFT_RIGHT(0b00, rd_, shamt); }

u16 C_SRAI(SmallReg rd_, u32 shamt) { return C_SHIFT_RIGHT(0b01, rd_, shamt); }

u16 C_ANDI(SmallReg rd_, u32 imm) {
    return cfunct3(0b100) | cbits(imm, 5, 5, 12) | (0b10 << 10) |
           cbits(rd_, 2, 0, 7) | cbits(imm, 4, 0, 2) | 0b01;
}
u16 C_MV(Reg rd, u32 rs2) {
    return (0b1000 << 12) | cbits(rd, 4, 0, 7) | cbits(rs2, 4, 0, 2) | 0b10;
}

u16 C_ADD(Reg rd, u32 rs2) {
    return cfunct3(0b100) | (1 << 12) | cbits(rd, 4, 0, 7) |
           cbits(rs2, 4, 0, 2) | 0b10;
}

static u16 C_ALU(SmallReg rd_, SmallReg rs2_, u32 funct2) {
    return (0b100011 << 10) | cbits(rd_, 2, 0, 7) | (u16)(funct2 << 5) |
           cbits(rs2_, 2, 0, 2) | 0b01;
}

u16 C_AND(SmallReg rd_, SmallReg rs2_) { return C_ALU(rd_, rs2_, 0b11); }

u16 C_OR(SmallReg rd_, SmallReg rs2_) { return C_ALU(rd_, rs2_, 0b10); }

u16 C_XOR(SmallReg rd_, SmallReg rs2_) { return C_ALU(rd_, rs2_, 0b01); }

u16 C_SUB(SmallReg rd_, SmallReg rs2_) { return C_ALU(rd_, rs2_, 0b00); }

u16 C_NOP(void) { return 0b01; }

u16 C_EBREAK(void) {
    u16 inst = 0;
    inst |= 0b1001 << 12;  // funct4
    inst |= 0b10;          // opcode
    return inst;
}

bool whitespace(char c) {
    return c == '\n' || c == '\t' || c == ' ' || c == '\r';
}
bool trailing(char c) { return c == '\t' || c == ' '; }

bool digit(char c) { return (c >= '0' && c <= '9'); }
bool ident_first(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c == '_') ||
           (c == '.');
}

bool ident(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || (c == '_') || (c == '.');
}

// the WASM version is freestanding, so reimplement ASCII tolower
char my_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

void advance(Parser *p) {
    if (p->pos >= p->size) return;
    if (p->input[p->pos] == '\n') p->lineidx++;
    p->pos++;
}

void advance_n(Parser *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        advance(p);
    }
}

char peek(Parser *p) {
    if (p->pos >= p->size) return '\0';
    return p->input[p->pos];
}

char peek_n(Parser *p, size_t n) {
    if (p->pos + n >= p->size) return '\0';
    return p->input[p->pos + n];
}

// the difference between the whitespace and trailing functions
// is that whitespace also includes newlines
// and as such can be done between tokens in a line
// for example
//     li x0,
//        1234
// whereas i need the trailing space to end the line gracefully
// otherwise i would be marking as valid stuff like
// li x0, 1234li x0, 1234

// Skip a single comment or preprocessor line if present.
// Returns true if a comment was skipped.
bool skip_comment(Parser *p) {
    char c = peek(p);
    if (c == '/') {
        char c2 = peek_n(p, 1);
        if (c2 == '/') {
            while (p->pos < p->size && p->input[p->pos] != '\n') advance(p);
            return true;
        } else if (c2 == '*') {
            advance_n(p, 2);
            while (p->pos < p->size && !(peek(p) == '*' && peek_n(p, 1) == '/'))
                advance(p);
            if (p->pos < p->size) advance_n(p, 2);
            return true;
        }
        return false;
    }
    if (c == '#') {
        while (p->pos < p->size && p->input[p->pos] != '\n') advance(p);
        return true;
    }
    return false;
}

void skip_whitespace(Parser *p) {
    while (p->pos < p->size) {
        if (whitespace(peek(p))) {
            advance(p);
        } else if (skip_comment(p)) {
        } else break;
    }
}
void skip_trailing(Parser *p) {
    while (p->pos < p->size) {
        if (trailing(peek(p))) {
            advance(p);
        } else if (skip_comment(p)) {
        } else break;
    }
}

bool consume_if(Parser *p, char c) {
    if (p->pos >= p->size) return false;
    if (p->input[p->pos] != c) return false;
    advance(p);
    return true;
}

bool consume(Parser *p, char *c) {
    if (p->pos >= p->size) return false;
    *c = p->input[p->pos];
    advance(p);
    return true;
}

bool parse_ident(Parser *p, const char **str, size_t *len) {
    size_t start = p->pos;
    if (p->pos >= p->size) {
        *str = NULL;
        *len = 0;
        return false;
    }
    if (!ident_first(peek(p))) {
        *str = NULL;
        *len = 0;
        return false;
    }
    while (ident(peek(p))) advance(p);
    size_t end = p->pos;
    *str = p->input + start;
    *len = end - start;
    return true;
}

bool str_eq(const char *txt, size_t len, const char *c) {
    if (len != strlen(c)) return false;
    for (size_t i = 0; i < len; i++) {
        if (c[i] != txt[i]) return false;
    }
    return true;
}

bool str_eq_case(const char *txt, size_t len, const char *c) {
    if (len != strlen(c)) return false;
    for (size_t i = 0; i < len; i++) {
        if (my_tolower(c[i]) != my_tolower(txt[i])) return false;
    }
    return true;
}

bool str_eq_2(const char *s1, size_t s1len, const char *s2, size_t s2len) {
    if (s1len != s2len) return false;
    // both 0. i need this to avoid memcmp(NULL, NULL, 0) UB
    if (s1len == 0) return true;
    return memcmp(s1, s2, s1len) == 0;
}

bool parse_numeric(Parser *p, i32 *out) {
    Parser start = *p;
    bool negative = false;
    bool parsed_digit = false;
    // NOTE: using a 64bit int to avoid 32bit overflow
    // if we ever support 64bit numbers, we need a smarter approach
    i64 value = 0;
    int base = 10;
    while (peek(p) == '-' || peek(p) == '+') {
        if (consume_if(p, '-')) negative = !negative;
        consume_if(p, '+');
    }

    if (consume_if(p, '\'')) {
        char c;
        if (!consume(p, &c)) {
            *p = start;
            return false;
        }
        if (c == '\\') {
            if (!consume(p, &c)) {
                *p = start;
                return false;
            }
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            else if (c == 'e') c = '\e';
            else if (c == 'f') c = '\f';
            else if (c == 'a') c = '\a';
            else if (c == 'b') c = '\b';
            else if (c == '\\') c = '\\';
            else if (c == '\'') c = '\'';
            else if (c == '"') c = '"';
            else if (c == '0') c = 0;
            else {
                *p = start;
                return false;
            }
        }
        value = (unsigned char)c;
        if (!consume_if(p, '\'')) {
            *p = start;
            return false;
        }
    } else {
        if (peek(p) == '0') {
            char prefix = peek_n(p, 1);
            if (prefix == 'x' || prefix == 'X') base = 16;
            else if (prefix == 'b' || prefix == 'B') base = 2;
            if (base != 10) advance_n(p, 2);
        }

        for (char c; (c = peek(p));) {
            int digit = base;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            if (digit >= base) {
                if (whitespace(c)) break;
                if (c == ' ' || c == '(' || c == ')' || c == ',' || c == '\0')
                    break;
                *p = start;
                return false;
            }
            parsed_digit = true;
            value = value * base + digit;
            // by giving an extremely long number
            // the user could overflow the i64 too
            if (value > 4294967295) {
                *p = start;
                return false;
            }
            advance(p);
        }
        if (!parsed_digit) {
            *p = start;
            return false;
        }
    }
    if (negative) value = -value;
    if (value < -2147483648LL || value > 4294967295LL) {
        *p = start;
        return false;
    }
    *out = value;
    return true;
}

bool parse_quoted_str(Parser *p, char **out_str, size_t *out_len) {
    ARES_ARRAY(char) buf = ARES_ARRAY_NEW(char);

    bool escape = false;
    if (!consume_if(p, '"')) {
        ARES_ARRAY_FREE(&buf);
        return false;
    }

    while (true) {
        char c = peek(p);
        if (c == 0) {
            ARES_ARRAY_FREE(&buf);
            return false;  // unquoted string
        }
        if (escape) {
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            else if (c == 'v') c = '\v';
            else if (c == 'f') c = '\f';
            else if (c == 'a') c = '\a';
            else if (c == 'b') c = '\b';
            else if (c == '\\') c = '\\';
            else if (c == '\'') c = '\'';
            else if (c == '"') c = '"';
            else if (c == '0') c = 0;
            else {
                ARES_ARRAY_FREE(&buf);
                return false;
            }
            *ARES_ARRAY_PUSH(&buf) = c;
            escape = false;
            advance(p);
            continue;
        }
        if (c == '\\') {
            escape = true;
            advance(p);
            continue;
        }
        if (c == '"') {
            advance(p);
            break;
        }
        *ARES_ARRAY_PUSH(&buf) = c;
        advance(p);
    }

    *out_str = buf.buf;
    *out_len = buf.len;
    return true;
}

int parse_reg(Parser *p) {
    const char *str;
    size_t len;
    parse_ident(p, &str, &len);

    if ((len == 2 || len == 3) && (str[0] == 'x' || str[0] == 'X')) {
        if (len == 2) {
            int num = str[1] - '0';
            if (num < 0 || num > 9) return -1;
            return num;
        } else {
            // 2-digit registers starting with 0 are invalid
            if (str[1] <= '0' || str[1] > '9') return -1;
            if (str[2] < '0' || str[2] > '9') return -1;
            int num = (str[1] - '0') * 10 + (str[2] - '0');
            if (num < 0 || num >= 32) return -1;
            return num;
        }
    }
    for (int i = 0; i < 32; i++) {
        if (str_eq_case(str, len, REGISTER_NAMES[i])) return i;
    }
    if (str_eq_case(str, len, "s0")) return 8;  // s0 = fp
    return -1;
}

int parse_csr(Parser *p) {
    Parser start = *p;
    i32 value;
    if (parse_numeric(p, &value)) {
        if (value >= 0 && value <= 0xfff) return value;
        *p = start;
        return -1;
    }

    const char *str;
    size_t len;
    parse_ident(p, &str, &len);

    for (size_t i = 0; i < sizeof(CSR_NAMES) / sizeof(CSR_NAMES[0]); i++) {
        if (CSR_NAMES[i] && str_eq_case(str, len, CSR_NAMES[i])) return i;
    }

    return -1;
}

void asm_emit_byte(u8 byte, int linenum) {
    if (!g_in_fixup) {
        *ARES_ARRAY_PUSH(&g_section->contents) = byte;
        *ARES_ARRAY_PUSH(&g_section->by_linenum) = linenum;
    } else {
        *ARES_ARRAY_WRITE_AT(&g_section->contents, g_section->emit_idx) = byte;
    }
    g_section->emit_idx++;
}

void asm_emit(u32 inst, int linenum) {
    asm_emit_byte(inst >> 0, linenum);
    asm_emit_byte(inst >> 8, linenum);
    asm_emit_byte(inst >> 16, linenum);
    asm_emit_byte(inst >> 24, linenum);
}

void asm_emit_16(u32 inst, int linenum) {
    asm_emit_byte(inst >> 0, linenum);
    asm_emit_byte(inst >> 8, linenum);
}

static Extern *get_extern(const char *sym, size_t sym_len) {
    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_externs); i++) {
        if (ARES_ARRAY_GET(&g_externs, i)->len == sym_len &&
            0 == memcmp(sym, ARES_ARRAY_GET(&g_externs, i)->symbol, sym_len)) {
            return ARES_ARRAY_GET(&g_externs, i);
        }
    }

    Extern *e = ARES_ARRAY_PUSH(&g_externs);
    e->symbol = sym;
    e->len = sym_len;
    return e;
}

const char *reloc_c_j(const char *sym, size_t sym_len) { return NULL; }

const char *reloc_branch(const char *sym, size_t sym_len) {
    Extern *e = get_extern(sym, sym_len);
    Relocation *r = ARES_ARRAY_PUSH(&g_section->relocations);
    r->symbol = e;
    r->addend = 0;
    r->offset = g_section->emit_idx;
    r->type = R_RISCV_BRANCH;
    return NULL;
}

const char *reloc_jal(const char *sym, size_t sym_len) {
    Extern *e = get_extern(sym, sym_len);
    Relocation *r = ARES_ARRAY_PUSH(&g_section->relocations);
    r->symbol = e;
    r->addend = 0;
    r->offset = g_section->emit_idx;
    r->type = R_RISCV_JAL;
    return NULL;
}

const char *reloc_hi20(const char *sym, size_t sym_len) {
    Extern *e = get_extern(sym, sym_len);
    Relocation *r = ARES_ARRAY_PUSH(&g_section->relocations);

    r->symbol = e;
    r->addend = 0;
    r->offset = g_section->emit_idx;
    r->type = R_RISCV_HI20;
    return NULL;
}

const char *reloc_lo12i(const char *sym, size_t sym_len) {
    Extern *e = get_extern(sym, sym_len);
    Relocation *r = ARES_ARRAY_PUSH(&g_section->relocations);

    r->symbol = e;
    r->addend = 0;
    r->offset = g_section->emit_idx;
    r->type = R_RISCV_LO12_I;
    return NULL;
}

const char *reloc_lo12s(const char *sym, size_t sym_len) {
    Extern *e = get_extern(sym, sym_len);
    Relocation *r = ARES_ARRAY_PUSH(&g_section->relocations);

    r->symbol = e;
    r->addend = 0;
    r->offset = g_section->emit_idx;
    r->type = R_RISCV_LO12_S;
    return NULL;
}

const char *reloc_hi20lo12i(const char *sym, size_t sym_len) {
    Extern *e = get_extern(sym, sym_len);
    Relocation *r = ARES_ARRAY_PUSH(&g_section->relocations);

    r->symbol = e;
    r->addend = 0;
    r->offset = g_section->emit_idx;
    r->type = R_RISCV_HI20;

    r = ARES_ARRAY_PUSH(&g_section->relocations);
    r->symbol = e;
    r->addend = 0;
    r->offset = g_section->emit_idx + 4;
    r->type = R_RISCV_LO12_I;
    return NULL;
}

const char *reloc_hi20lo12s(const char *sym, size_t sym_len) {
    Extern *e = get_extern(sym, sym_len);
    Relocation *r = ARES_ARRAY_PUSH(&g_section->relocations);

    r->symbol = e;
    r->addend = 0;
    r->offset = g_section->emit_idx;
    r->type = R_RISCV_HI20;

    r = ARES_ARRAY_PUSH(&g_section->relocations);
    r->symbol = e;
    r->addend = 0;
    r->offset = g_section->emit_idx + 4;
    r->type = R_RISCV_LO12_S;
    return NULL;
}

const char *reloc_abs32(const char *sym, size_t sym_len) {
    Extern *e = get_extern(sym, sym_len);
    Relocation *r = ARES_ARRAY_PUSH(&g_section->relocations);

    r->symbol = e;
    r->addend = 0;
    r->offset = g_section->emit_idx;
    r->type = R_RISCV_32;
    return NULL;
}

static const char *reloc_pcrel_hi20lo12i(const char *sym, size_t sym_len) {
    size_t local_label_idx = ARES_ARRAY_LEN(&g_local_labels);
    LocalLabel *lbl = ARES_ARRAY_PUSH(&g_local_labels);
    lbl->section = g_section;
    lbl->offset = g_section->emit_idx;

    Relocation *r_hi = ARES_ARRAY_PUSH(&g_section->relocations);
    r_hi->kind = RELOCATION_KIND_EXTERN;
    r_hi->symbol = get_extern(sym, sym_len);
    r_hi->addend = 0;
    r_hi->offset = g_section->emit_idx;
    r_hi->type = R_RISCV_PCREL_HI20;

    Relocation *r_lo = ARES_ARRAY_PUSH(&g_section->relocations);
    r_lo->kind = RELOCATION_KIND_LOCAL_LABEL;
    r_lo->local_label_idx = local_label_idx;
    r_lo->addend = 0;
    r_lo->offset = g_section->emit_idx + 4;
    r_lo->type = R_RISCV_PCREL_LO12_I;

    return NULL;
}

static const char *reloc_pcrel_hi20(const char *sym, size_t sym_len) {
    Relocation *r_hi = ARES_ARRAY_PUSH(&g_section->relocations);
    r_hi->kind = RELOCATION_KIND_EXTERN;
    r_hi->symbol = get_extern(sym, sym_len);
    r_hi->addend = 0;
    r_hi->offset = g_section->emit_idx;
    r_hi->type = R_RISCV_PCREL_HI20;
    return NULL;
}

static const char *reloc_pcrel_lo12i(const char *sym, size_t sym_len) {
    Relocation *r_lo = ARES_ARRAY_PUSH(&g_section->relocations);
    r_lo->kind = RELOCATION_KIND_EXTERN;
    r_lo->symbol = get_extern(sym, sym_len);
    r_lo->addend = 0;
    r_lo->offset = g_section->emit_idx;
    r_lo->type = R_RISCV_PCREL_LO12_I;
    return NULL;
}

static const char *reloc_pcrel_lo12s(const char *sym, size_t sym_len) {
    Relocation *r_lo = ARES_ARRAY_PUSH(&g_section->relocations);
    r_lo->kind = RELOCATION_KIND_EXTERN;
    r_lo->symbol = get_extern(sym, sym_len);
    r_lo->addend = 0;
    r_lo->offset = g_section->emit_idx;
    r_lo->type = R_RISCV_PCREL_LO12_S;
    return NULL;
}

const char *label(Parser *p, Parser *orig, DeferredInsnCb *cb,
                  const char *opcode, size_t opcode_len, u32 *out_addr,
                  bool *later, DeferredInsnReloc *reloc) {
    *later = false;

    const char *target;
    size_t target_len;
    parse_ident(p, &target, &target_len);
    if (target_len == 0) return "No label";

    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_labels); i++) {
        if (str_eq_2(ARES_ARRAY_GET(&g_labels, i)->txt,
                     ARES_ARRAY_GET(&g_labels, i)->len, target, target_len)) {
            *out_addr = ARES_ARRAY_GET(&g_labels, i)->addr;
            return NULL;
        }
    }

    if (g_in_fixup && (!reloc || !g_allow_externs)) return "Label not found";
    if (g_in_fixup) {
        *out_addr = 0;
        return reloc(target, target_len);
    }
    DeferredInsn *insn = ARES_ARRAY_PUSH(&g_deferred_insn);
    insn->emit_idx = g_section->emit_idx;
    insn->p = *orig;
    insn->cb = cb;
    insn->opcode = opcode;
    insn->opcode_len = opcode_len;
    insn->section = g_section;
    *later = true;
    return NULL;
}

const char *pc_relative_target(Parser *p, Parser *orig, DeferredInsnCb *cb,
                               const char *opcode, size_t opcode_len,
                               u32 *out_addr, bool *later,
                               DeferredInsnReloc *reloc) {
    *later = false;

    i32 off;
    Parser fallback = *p;
    if (parse_numeric(p, &off)) {
        *out_addr = (u32)(g_section->emit_idx + g_section->base + off);
        return NULL;
    }
    *p = fallback;

    return label(p, orig, cb, opcode, opcode_len, out_addr, later, reloc);
}

const char *parse_modifier_hi(Parser *p, Parser orig, DeferredInsnCb *cb,
                              const char *opcode, size_t opcode_len,
                              i32 *simm) {
    const char *modifier;
    size_t modifier_len;
    u32 addr;
    i32 num;
    parse_ident(p, &modifier, &modifier_len);
    if (!consume_if(p, '(')) return "Expected (";
    DeferredInsnReloc *reloc = NULL;
    if (str_eq_case(modifier, modifier_len, "hi")) reloc = reloc_hi20;
    else if (str_eq_case(modifier, modifier_len, "pcrel_hi"))
        reloc = reloc_pcrel_hi20;
    else return "Invalid modifier";

    if (parse_numeric(p, &num)) {
        addr = num;
    } else {
        bool later;
        const char *err =
            label(p, &orig, cb, opcode, opcode_len, &addr, &later, reloc);
        if (err) return err;
        if (later) {
            if (!consume_if(p, ')')) return "Expected )";
            *simm = 0;
            return NULL;
        }
    }
    if (!consume_if(p, ')')) return "Expected )";
    if (reloc == reloc_pcrel_hi20) {
        *ARES_ARRAY_PUSH(&g_pcrel_hi_relocs) =
            (PcrelHiReloc){.label_addr = g_section->emit_idx + g_section->base,
                           .dest_addr = addr};
        addr -= g_section->emit_idx + g_section->base;
    }
    i32 lo = (i32)((u32)addr << 20) >> 20;
    u32 hi = (u32)(addr - lo) >> 12;
    *simm = hi;
    return NULL;
}

const char *parse_modifier_lo(Parser *p, Parser orig, bool is_i,
                              DeferredInsnCb *cb, const char *opcode,
                              size_t opcode_len, i32 *simm) {
    const char *modifier;
    size_t modifier_len;
    u32 addr;
    i32 num;
    parse_ident(p, &modifier, &modifier_len);
    if (!consume_if(p, '(')) return "Expected (";
    DeferredInsnReloc *reloc = NULL;
    if (str_eq_case(modifier, modifier_len, "lo"))
        reloc = is_i ? reloc_lo12i : reloc_lo12s;
    else if (str_eq_case(modifier, modifier_len, "pcrel_lo"))
        reloc = is_i ? reloc_pcrel_lo12i : reloc_pcrel_lo12s;
    else return "Invalid modifier";

    if (parse_numeric(p, &num)) {
        addr = num;
    } else {
        bool later;
        const char *err =
            label(p, &orig, cb, opcode, opcode_len, &addr, &later, reloc);
        if (err) return err;
        if (later) {
            if (!consume_if(p, ')')) return "Expected )";
            *simm = 0;
            return NULL;
        }
    }
    if (!consume_if(p, ')')) return "Expected )";
    if (reloc == reloc_pcrel_lo12i || reloc == reloc_pcrel_lo12s) {
        bool found = false;
        for (size_t i = 0; i < g_pcrel_hi_relocs.len; i++) {
            if (g_pcrel_hi_relocs.buf[i].label_addr == addr) {
                addr = g_pcrel_hi_relocs.buf[i].dest_addr - addr;
                found = true;
            }
        }
        if (!found) {
            if (!g_in_fixup) {
                DeferredInsn *insn = ARES_ARRAY_PUSH(&g_deferred_insn);
                insn->emit_idx = g_section->emit_idx;
                insn->p = orig;
                insn->cb = cb;
                insn->opcode = opcode;
                insn->opcode_len = opcode_len;
                insn->section = g_section;
                *simm = 0;
                return NULL;
            } else {
                // TODO: this also includes places where it is in a weird order
                // like this:
                // addi a0, a0, %pcrel_lo(l0)
                // l0:
                // auipc a0, %pcrel_hi(label)
                // label:
                return "Invalid pcrel label";
            }
        }
    }
    i32 lo = (i32)((u32)addr << 20) >> 20;
    *simm = lo;
    return NULL;
}

const char *handle_alu_reg(Parser *p, const char *opcode, size_t opcode_len) {
    int d, s1, s2;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if ((s1 = parse_reg(p)) == -1) return "Invalid rs1";
    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if ((s2 = parse_reg(p)) == -1) return "Invalid rs2";

    u32 inst = 0;
    if (str_eq_case(opcode, opcode_len, "add")) inst = ADD(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "slt")) inst = SLT(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "sltu")) inst = SLTU(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "and")) inst = AND(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "or")) inst = OR(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "xor")) inst = XOR(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "sll")) inst = SLL(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "srl")) inst = SRL(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "sub")) inst = SUB(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "sra")) inst = SRA(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "mul")) inst = MUL(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "mulh")) inst = MULH(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "mulhsu"))
        inst = MULHSU(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "mulhu")) inst = MULHU(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "div")) inst = DIV(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "divu")) inst = DIVU(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "rem")) inst = REM(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "remu")) inst = REMU(d, s1, s2);

    asm_emit(inst, p->startline);
    return NULL;
}

const char *handle_ext(Parser *p, const char *opcode, size_t opcode_len) {
    int d, s1;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if ((s1 = parse_reg(p)) == -1) return "Invalid rs1";
    skip_trailing(p);

    if (str_eq_case(opcode, opcode_len, "zext.b")) {
        asm_emit(ANDI(d, s1, 255), p->startline);
    } else if (str_eq_case(opcode, opcode_len, "zext.h")) {
        asm_emit(SLLI(d, s1, 16), p->startline);
        asm_emit(SRLI(d, d, 16), p->startline);
    } else if (str_eq_case(opcode, opcode_len, "sext.b")) {
        asm_emit(SLLI(d, s1, 24), p->startline);
        asm_emit(SRAI(d, d, 24), p->startline);
    } else if (str_eq_case(opcode, opcode_len, "sext.h")) {
        asm_emit(SLLI(d, s1, 16), p->startline);
        asm_emit(SRAI(d, d, 16), p->startline);
    }

    return NULL;
}

const char *handle_alu_imm(Parser *p, const char *opcode, size_t opcode_len) {
    Parser orig = *p;
    int d, s1;
    i32 simm;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if ((s1 = parse_reg(p)) == -1) return "Invalid rs1";
    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if (consume_if(p, '%')) {
        const char *err = parse_modifier_lo(p, orig, true, handle_alu_imm,
                                            opcode, opcode_len, &simm);
        if (err) return err;
    } else if (!parse_numeric(p, &simm)) return "Invalid immediate";
    if (simm < -2048 || simm > 2047) return "Out of bounds immediate";
    bool is_shift = str_eq_case(opcode, opcode_len, "slli") ||
                    str_eq_case(opcode, opcode_len, "srli") ||
                    str_eq_case(opcode, opcode_len, "srai");
    if (is_shift && (simm < 0 || simm >= 32)) return "Invalid shift immediate";
    u32 inst = 0;
    if (str_eq_case(opcode, opcode_len, "addi")) inst = ADDI(d, s1, simm);
    else if (str_eq_case(opcode, opcode_len, "slti")) inst = SLTI(d, s1, simm);
    else if (str_eq_case(opcode, opcode_len, "sltiu"))
        inst = SLTIU(d, s1, simm);
    else if (str_eq_case(opcode, opcode_len, "andi")) inst = ANDI(d, s1, simm);
    else if (str_eq_case(opcode, opcode_len, "ori")) inst = ORI(d, s1, simm);
    else if (str_eq_case(opcode, opcode_len, "xori")) inst = XORI(d, s1, simm);
    else if (str_eq_case(opcode, opcode_len, "slli")) inst = SLLI(d, s1, simm);
    else if (str_eq_case(opcode, opcode_len, "srli")) inst = SRLI(d, s1, simm);
    else if (str_eq_case(opcode, opcode_len, "srai")) inst = SRAI(d, s1, simm);

    asm_emit(inst, p->startline);

    return NULL;
}

const char *handle_ldst(Parser *p, const char *opcode, size_t opcode_len) {
    Parser orig = *p;
    int reg, mem;
    i32 simm;

    bool store = str_eq_case(opcode, opcode_len, "sb") ||
                 str_eq_case(opcode, opcode_len, "sh") ||
                 str_eq_case(opcode, opcode_len, "sw");

    skip_trailing(p);
    if ((reg = parse_reg(p)) == -1) return "Invalid rreg";
    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);

    if (consume_if(p, '%')) {
        const char *err = parse_modifier_lo(p, orig, !store, handle_ldst,
                                            opcode, opcode_len, &simm);
        if (err) return err;
    } else if (!parse_numeric(p, &simm)) return "Invalid immediate";
    if (simm < -2048 || simm > 2047) return "Out of bounds immediate";

    skip_trailing(p);
    if (!consume_if(p, '(')) return "Expected (";
    skip_trailing(p);
    if ((mem = parse_reg(p)) == -1) return "Invalid rmem";
    skip_trailing(p);
    if (!consume_if(p, ')')) return "Expected )";

    u32 inst = 0;
    if (str_eq_case(opcode, opcode_len, "lb")) inst = LB(reg, mem, simm);
    else if (str_eq_case(opcode, opcode_len, "lh")) inst = LH(reg, mem, simm);
    else if (str_eq_case(opcode, opcode_len, "lw")) inst = LW(reg, mem, simm);
    else if (str_eq_case(opcode, opcode_len, "lbu")) inst = LBU(reg, mem, simm);
    else if (str_eq_case(opcode, opcode_len, "lhu")) inst = LHU(reg, mem, simm);
    else if (str_eq_case(opcode, opcode_len, "sb")) inst = SB(reg, mem, simm);
    else if (str_eq_case(opcode, opcode_len, "sh")) inst = SH(reg, mem, simm);
    else if (str_eq_case(opcode, opcode_len, "sw")) inst = SW(reg, mem, simm);

    asm_emit(inst, p->startline);
    return NULL;
}

static u32 branch_inst(const char *opcode, size_t opcode_len, int s1, int s2,
                       i32 simm) {
    if (str_eq_case(opcode, opcode_len, "beq")) return BEQ(s1, s2, simm);
    if (str_eq_case(opcode, opcode_len, "bne")) return BNE(s1, s2, simm);
    if (str_eq_case(opcode, opcode_len, "blt")) return BLT(s1, s2, simm);
    if (str_eq_case(opcode, opcode_len, "bge")) return BGE(s1, s2, simm);
    if (str_eq_case(opcode, opcode_len, "bltu")) return BLTU(s1, s2, simm);
    if (str_eq_case(opcode, opcode_len, "bgeu")) return BGEU(s1, s2, simm);
    if (str_eq_case(opcode, opcode_len, "bgt")) return BLT(s2, s1, simm);
    if (str_eq_case(opcode, opcode_len, "ble")) return BGE(s2, s1, simm);
    if (str_eq_case(opcode, opcode_len, "bgtu")) return BLTU(s2, s1, simm);
    if (str_eq_case(opcode, opcode_len, "bleu")) return BGEU(s2, s1, simm);
    return 0;
}

const char *handle_branch(Parser *p, const char *opcode, size_t opcode_len) {
    Parser orig = *p;
    u32 addr;
    int s1, s2;
    bool later;

    skip_trailing(p);
    if ((s1 = parse_reg(p)) == -1) return "Invalid rs1";
    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if ((s2 = parse_reg(p)) == -1) return "Invalid rs2";
    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    const char *err =
        pc_relative_target(p, &orig, handle_branch, opcode, opcode_len, &addr,
                           &later, reloc_branch);
    if (err) return err;
    if (later) {
        asm_emit(0, p->startline);
        return NULL;
    }
    i32 simm = addr - (g_section->emit_idx + g_section->base);
    if (simm >= (1 << 12) || simm < -(1 << 12))
        return "Branch immediate too large";
    if (simm & 1) return "Branch target must be even";

    asm_emit(branch_inst(opcode, opcode_len, s1, s2, simm), p->startline);
    return NULL;
}

static u32 branch_zero_inst(const char *opcode, size_t opcode_len, int s,
                            i32 simm) {
    if (str_eq_case(opcode, opcode_len, "beqz")) return BEQ(s, 0, simm);
    if (str_eq_case(opcode, opcode_len, "bnez")) return BNE(s, 0, simm);
    if (str_eq_case(opcode, opcode_len, "blez")) return BGE(0, s, simm);
    if (str_eq_case(opcode, opcode_len, "bgez")) return BGE(s, 0, simm);
    if (str_eq_case(opcode, opcode_len, "bltz")) return BLT(s, 0, simm);
    if (str_eq_case(opcode, opcode_len, "bgtz")) return BLT(0, s, simm);
    return 0;
}

const char *handle_branch_zero(Parser *p, const char *opcode,
                               size_t opcode_len) {
    Parser orig = *p;
    u32 addr;
    int s;
    bool later;

    skip_trailing(p);
    if ((s = parse_reg(p)) == -1) return "Invalid rs";
    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    const char *err =
        pc_relative_target(p, &orig, handle_branch_zero, opcode, opcode_len,
                           &addr, &later, reloc_branch);
    if (err) return err;
    if (later) {
        asm_emit(0, p->startline);
        return NULL;
    }
    i32 simm = addr - (g_section->emit_idx + g_section->base);
    if (simm >= (1 << 12) || simm < -(1 << 12))
        return "Branch immediate too large";
    if (simm & 1) return "Branch target must be even";

    asm_emit(branch_zero_inst(opcode, opcode_len, s, simm), p->startline);
    return NULL;
}

const char *handle_alu_pseudo(Parser *p, const char *opcode,
                              size_t opcode_len) {
    int d, s;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if ((s = parse_reg(p)) == -1) return "Invalid rs";

    u32 inst = 0;
    if (str_eq_case(opcode, opcode_len, "mv")) inst = ADDI(d, s, 0);
    else if (str_eq_case(opcode, opcode_len, "not")) inst = XORI(d, s, -1);
    else if (str_eq_case(opcode, opcode_len, "neg")) inst = SUB(d, 0, s);
    else if (str_eq_case(opcode, opcode_len, "seqz")) inst = SLTIU(d, s, 1);
    else if (str_eq_case(opcode, opcode_len, "snez")) inst = SLTU(d, 0, s);
    else if (str_eq_case(opcode, opcode_len, "sltz")) inst = SLT(d, s, 0);
    else if (str_eq_case(opcode, opcode_len, "sgtz")) inst = SLT(d, 0, s);

    asm_emit(inst, p->startline);
    return NULL;
}

const char *handle_jump(Parser *p, const char *opcode, size_t opcode_len) {
    int d;
    Parser orig = *p;
    const char *err = NULL;
    bool later;

    skip_trailing(p);
    // jal optionally takes a register argument
    if (str_eq_case(opcode, opcode_len, "jal") ||
        str_eq_case(opcode, opcode_len, "call")) {
        if ((d = parse_reg(p)) == -1) err = "Invalid rd";
        skip_trailing(p);
        if (consume_if(p, ',')) {
            if (err) return err;
        } else {
            *p = orig;
            d = 1;
        }
    } else if (str_eq_case(opcode, opcode_len, "j")) {
        d = 0;
    } else assert(false);

    skip_trailing(p);

    u32 addr;
    err = pc_relative_target(p, &orig, handle_jump, opcode, opcode_len, &addr,
                             &later, reloc_jal);
    if (err) return err;
    if (later) {
        asm_emit(0, p->startline);
        return NULL;
    }
    i32 simm = addr - (g_section->emit_idx + g_section->base);
    if (simm >= (1 << 20) || simm < -(1 << 20))
        return "Jump immediate too large";
    if (simm & 1) return "Jump target must be even";
    asm_emit(JAL(d, simm), p->startline);
    return NULL;
}

const char *handle_jump_reg(Parser *p, const char *opcode, size_t opcode_len) {
    int d, s;
    i32 simm;

    skip_trailing(p);
    // jalr rs
    // jalr rd, rs
    // jalr rd, rs, simm
    // jalr rd, simm(rs)
    if (str_eq_case(opcode, opcode_len, "jalr")) {
        if ((d = parse_reg(p)) == -1) return "Invalid register";
        skip_trailing(p);
        if (!consume_if(p, ',')) {
            asm_emit(JALR(1, d, 0), p->startline);
            return NULL;
        }
        skip_trailing(p);
        if (parse_numeric(p, &simm)) {  // simm(rs)
            skip_trailing(p);
            if (!consume_if(p, '(')) return "Expected (";
            skip_trailing(p);
            if ((s = parse_reg(p)) == -1) return "Invalid rs";
            skip_trailing(p);
            if (!consume_if(p, ')')) return "Expected )";
        } else if (consume_if(p, '(')) {  // (rs)
            simm = 0;
            skip_trailing(p);
            if ((s = parse_reg(p)) == -1) return "Invalid rs";
            skip_trailing(p);
            if (!consume_if(p, ')')) return "Expected )";
        } else if ((s = parse_reg(p)) != -1) {  // rd, rs / rd, rs, simm
            skip_trailing(p);
            if (consume_if(p, ',')) {  // rd, rs, simm
                skip_trailing(p);
                if (!parse_numeric(p, &simm)) return "Invalid immediate";
            } else {  // rd, rs
                simm = 0;
            }
        } else {
            return "Invalid operand";
        }
        if (simm >= -2048 && simm <= 2047)
            asm_emit(JALR(d, s, simm), p->startline);
        else return "Immediate out of range";
    } else if (str_eq_case(opcode, opcode_len, "jr")) {
        if ((s = parse_reg(p)) == -1) return "Invalid rs";
        asm_emit(JALR(0, s, 0), p->startline);
    }
    return NULL;
}

const char *handle_ret(Parser *p, const char *opcode, size_t opcode_len) {
    asm_emit(JALR(0, 1, 0), p->startline);
    return NULL;
}

const char *handle_upper(Parser *p, const char *opcode, size_t opcode_len) {
    Parser orig = *p;
    int d;
    i32 simm;
    u32 inst = 0;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";
    skip_trailing(p);
    if (consume_if(p, '%')) {
        const char *err =
            parse_modifier_hi(p, orig, handle_upper, opcode, opcode_len, &simm);
        if (err) return err;
    } else if (!parse_numeric(p, &simm)) return "Invalid immediate";
    // the immediate can either be signed or unsigned 20 bit
    if (simm < -524288 || simm > 1048575) return "Out of bounds immediate";

    if (str_eq_case(opcode, opcode_len, "lui")) inst = LUI(d, simm);
    else if (str_eq_case(opcode, opcode_len, "auipc")) inst = AUIPC(d, simm);

    asm_emit(inst, p->startline);
    return NULL;
}

const char *handle_li(Parser *p, const char *opcode, size_t opcode_len) {
    int d;
    i32 simm;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";
    skip_trailing(p);
    if (!parse_numeric(p, &simm)) return "Invalid immediate";

    if (simm >= -2048 && simm <= 2047) {
        asm_emit(ADDI(d, 0, simm), p->startline);
    } else {
        u32 lo = simm & 0xFFF;
        if (lo >= 0x800) lo -= 0x1000;
        u32 hi = (u32)(simm - lo) >> 12;
        asm_emit(LUI(d, hi), p->startline);
        if (lo != 0) asm_emit(ADDI(d, d, lo), p->startline);
    }
    return NULL;
}

const char *handle_la(Parser *p, const char *opcode, size_t opcode_len) {
    Parser orig = *p;
    int d;
    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";
    skip_trailing(p);

    u32 addr;
    bool later;
    const char *err = label(p, &orig, handle_la, opcode, opcode_len, &addr,
                            &later, reloc_pcrel_hi20lo12i);
    if (later) {
        asm_emit(0, p->startline);
        asm_emit(0, p->startline);
        return NULL;
    }
    if (err) return err;
    i32 pc = (i32)(g_section->emit_idx + g_section->base);
    i32 simm = (i32)addr - pc;

    i32 lo = (i32)((u32)simm << 20) >> 20;
    u32 hi = (u32)(simm - lo) >> 12;

    asm_emit(AUIPC(d, hi), p->startline);
    asm_emit(ADDI(d, d, lo), p->startline);
    return NULL;
}

const char *handle_ecall(Parser *p, const char *opcode, size_t opcode_len) {
    asm_emit(0x73, p->startline);
    return NULL;
}

const char *handle_ebreak(Parser *p, const char *opcode, size_t opcode_len) {
    asm_emit(0x00100073, p->startline);
    return NULL;
}

const char *handle_nop(Parser *p, const char *opcode, size_t opcode_len) {
    asm_emit(ADDI(0, 0, 0), p->startline);
    return NULL;
}

const char *handle_sret(Parser *p, const char *opcode, size_t opcode_len) {
    asm_emit(0x10200073, p->startline);
    return NULL;
}

const char *handle_csr(Parser *p, const char *opcode, size_t opcode_len) {
    int csr, d, s;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";

    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if ((csr = parse_csr(p)) == -1) return "Invalid CSR";

    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if ((s = parse_reg(p)) == -1) return "Invalid rs";

    u32 inst = 0;
    if (str_eq_case(opcode, opcode_len, "csrrw")) inst = CSRRW(d, s, csr);
    else if (str_eq_case(opcode, opcode_len, "csrrs")) inst = CSRRS(d, s, csr);
    else if (str_eq_case(opcode, opcode_len, "csrrc")) inst = CSRRC(d, s, csr);

    asm_emit(inst, p->startline);
    return NULL;
}

const char *handle_csr_imm(Parser *p, const char *opcode, size_t opcode_len) {
    int csr, d;
    i32 zimm;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";

    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if ((csr = parse_csr(p)) == -1) return "Invalid CSR";

    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if (!parse_numeric(p, &zimm)) return "Invalid immediate";

    u32 inst = 0;
    if (str_eq_case(opcode, opcode_len, "csrrwi")) inst = CSRRWI(d, zimm, csr);
    else if (str_eq_case(opcode, opcode_len, "csrrsi"))
        inst = CSRRSI(d, zimm, csr);
    else if (str_eq_case(opcode, opcode_len, "csrrci"))
        inst = CSRRCI(d, zimm, csr);

    asm_emit(inst, p->startline);
    return NULL;
}
// compressed handlers

const char *handle_c_lwsp(Parser *p, const char *opcode, size_t opcode_len) {
    Reg d, s;
    i32 simm;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    if (d == 0) return "rd cannot be x0 for c.lwsp: reserved";

    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if (!parse_numeric(p, &simm)) return "Invalid immediate";
    if (simm < 0 || simm > 255) return "Out of bounds immediate";
    if (simm % 4 != 0) return "Immediate must be a multiple of 4";

    skip_trailing(p);
    if (!consume_if(p, '(')) return "Expected (";

    skip_trailing(p);
    if ((s = parse_reg(p)) == -1) return "Invalid rs1";
    if (s != REG_SP) return "register must be sp";

    skip_trailing(p);
    if (!consume_if(p, ')')) return "Expected )";

    asm_emit_16(C_LWSP(d, simm), p->startline);
    return NULL;
}

const char *handle_c_swsp(Parser *p, const char *opcode, size_t opcode_len) {
    Reg s2, s;
    i32 simm;

    skip_trailing(p);
    if ((s2 = parse_reg(p)) == -1) return "Invalid rs2";

    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if (!parse_numeric(p, &simm)) return "Invalid immediate";
    if (simm < 0 || simm > 255) return "Out of bounds immediate";
    if (simm % 4 != 0) return "Immediate must be a multiple of 4";

    skip_trailing(p);
    if (!consume_if(p, '(')) return "Expected (";

    skip_trailing(p);
    if ((s = parse_reg(p)) == -1) return "Invalid rs1";
    if (s != REG_SP) return "register must be sp";

    skip_trailing(p);
    if (!consume_if(p, ')')) return "Expected )";

    asm_emit_16(C_SWSP(s2, simm), p->startline);
    return NULL;
}

const char *handle_c_lw(Parser *p, const char *opcode, size_t opcode_len) {
    Reg d, s1;
    SmallReg cd, cs1;
    i32 simm;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    if (d < 8 || d > 15) return "rd must be x8..x15 for c.lw";

    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if (!parse_numeric(p, &simm)) return "Invalid immediate";
    if (simm < 0 || simm > 127) return "Out of bounds immediate";
    if (simm % 4 != 0) return "Immediate must be a multiple of 4";

    skip_trailing(p);
    if (!consume_if(p, '(')) return "Expected (";

    skip_trailing(p);
    if ((s1 = parse_reg(p)) == -1) return "Invalid rs1";
    if (s1 < 8 || s1 > 15) return "rs1 must be x8..x15 for c.lw";

    skip_trailing(p);
    if (!consume_if(p, ')')) return "Expected )";

    cd = d - 8;
    cs1 = s1 - 8;

    asm_emit_16(C_LW(cd, cs1, simm), p->startline);
    return NULL;
}

const char *handle_c_sw(Parser *p, const char *opcode, size_t opcode_len) {
    Reg s2, s1;
    SmallReg cs2, cs1;
    i32 simm;

    skip_trailing(p);
    if ((s2 = parse_reg(p)) == -1) return "Invalid rs2";
    if (s2 < 8 || s2 > 15) return "rs2 must be x8..x15 for c.sw";

    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if (!parse_numeric(p, &simm)) return "Invalid immediate";
    if (simm < 0 || simm > 127) return "Out of bounds immediate";
    if (simm % 4 != 0) return "Immediate must be a multiple of 4";

    skip_trailing(p);
    if (!consume_if(p, '(')) return "Expected (";

    skip_trailing(p);
    if ((s1 = parse_reg(p)) == -1) return "Invalid rs1";
    if (s1 < 8 || s1 > 15) return "rs1 must be x8..x15 for c.sw";

    skip_trailing(p);
    if (!consume_if(p, ')')) return "Expected )";

    cs2 = s2 - 8;
    cs1 = s1 - 8;

    asm_emit_16(C_SW(cs2, cs1, simm), p->startline);
    return NULL;
}

const char *handle_c_jump(Parser *p, const char *opcode, size_t opcode_len) {
    Parser orig = *p;
    u32 addr;
    bool later;

    skip_trailing(p);
    const char *err = pc_relative_target(p, &orig, handle_c_jump, opcode,
                                         opcode_len, &addr, &later, reloc_c_j);
    if (err) return err;
    if (later) {
        asm_emit_16(0, p->startline);
        return NULL;
    }

    i32 simm = addr - (g_section->emit_idx + g_section->base);
    if (simm >= (1 << 11) || simm < -(1 << 11))
        return "Jump immediate too large";
    if (simm & 1) return "Jump target must be even";

    if (str_eq_case(opcode, opcode_len, "c.j")) {
        asm_emit_16(C_J(simm), p->startline);
    } else if (str_eq_case(opcode, opcode_len, "c.jal")) {
        asm_emit_16(C_JAL(simm), p->startline);
    }

    return NULL;
}

const char *handle_c_jump_reg(Parser *p, const char *opcode,
                              size_t opcode_len) {
    Reg rs1;

    skip_trailing(p);
    if ((rs1 = parse_reg(p)) == (Reg)-1) return "Invalid rs1";

    if (str_eq_case(opcode, opcode_len, "c.jr")) {
        if (rs1 == 0) return "rs1 cannot be x0 for c.jr: reserved";
        asm_emit_16(C_JR(rs1), p->startline);
    } else if (str_eq_case(opcode, opcode_len, "c.jalr")) {
        if (rs1 == 0) return "c.jalr rd, off(x0) corresponds to c.ebreak";
        asm_emit_16(C_JALR(rs1), p->startline);
    }

    return NULL;
}

const char *handle_c_branch_zero(Parser *p, const char *opcode,
                                 size_t opcode_len) {
    Parser orig = *p;
    u32 addr;
    Reg s1;
    SmallReg cs1;
    bool later;

    skip_trailing(p);
    if ((s1 = parse_reg(p)) == -1) return "Invalid rs1";
    if (s1 < 8 || s1 > 15) return "rs1 must be x8..x15";

    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    const char *err =
        pc_relative_target(p, &orig, handle_c_branch_zero, opcode, opcode_len,
                           &addr, &later, reloc_branch);
    if (err) return err;
    if (later) {
        asm_emit_16(0, p->startline);
        return NULL;
    }

    i32 simm = addr - (g_section->emit_idx + g_section->base);
    if (simm >= 256 || simm < -256) return "Branch immediate too large";
    if (simm & 1) return "Branch target must be even";

    cs1 = s1 - 8;

    if (str_eq_case(opcode, opcode_len, "c.beqz")) {
        asm_emit_16(C_BEQZ(cs1, simm), p->startline);
    } else if (str_eq_case(opcode, opcode_len, "c.bnez")) {
        asm_emit_16(C_BNEZ(cs1, simm), p->startline);
    }

    return NULL;
}

const char *handle_c_li(Parser *p, const char *opcode, size_t opcode_len) {
    Reg d;
    i32 simm;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    if (d == 0) return "rd cannot be x0 for c.li: HINT";

    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if (!parse_numeric(p, &simm)) return "Invalid immediate";
    if (simm < -32 || simm > 31) return "Out of bounds immediate";

    asm_emit_16(C_LI(d, simm), p->startline);
    return NULL;
}

const char *handle_c_lui(Parser *p, const char *opcode, size_t opcode_len) {
    Reg d;
    i32 simm;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    if (d == 0) return "rd cannot be x0 for c.lui: HINT";

    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if (!parse_numeric(p, &simm)) return "Invalid immediate";
    if (simm < -32 || simm > 31) return "Out of bounds immediate";
    if (simm == 0) return "Immediate cannot be 0 for c.lui: reserved ";
    if (d == 2) return "c.lui x2, imm corresponds to c.addi16sp";

    asm_emit_16(C_LUI(d, simm), p->startline);
    return NULL;
}

const char *handle_c_addi(Parser *p, const char *opcode, size_t opcode_len) {
    Reg d;
    i32 simm;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if (!parse_numeric(p, &simm)) return "Invalid immediate";
    if (simm < -32 || simm > 31) return "Out of bounds immediate";

    if (d == 0 && simm == 0) return "c.addi x0, 0 corresponds to c.nop";
    if (d == 0 && simm != 0)
        return "rd x0, imm is not a valid instruction: HINT";
    if (simm == 0) return "Immediate cannot be 0 for c.addi: HINT";

    asm_emit_16(C_ADDI(d, simm), p->startline);
    return NULL;
}

const char *handle_c_addi16sp(Parser *p, const char *opcode,
                              size_t opcode_len) {
    Reg d;
    i32 simm;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    if (d != 2) return "rd must be x2 for c.addi16sp";

    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if (!parse_numeric(p, &simm)) return "Invalid immediate";
    if (simm < -512 || simm > 496) return "Out of bounds immediate";
    if (simm == 0) return "Immediate can't be 0 for c.addi16sp: reserved";
    if (simm % 16 != 0) return "Immediate must be a multiple of 16";

    asm_emit_16(C_ADDI16SP(simm), p->startline);
    return NULL;
}

const char *handle_c_addi4spn(Parser *p, const char *opcode,
                              size_t opcode_len) {
    Reg d;
    SmallReg cd;
    i32 simm;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    if (d < 8 || d > 15) return "rd must be x8..x15 for c.addi4spn";

    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if (!parse_numeric(p, &simm)) return "Invalid immediate";
    if (simm <= 0 || simm > 1020) return "Out of bounds immediate";
    if (simm == 0) return "Immediate cannot be 0 for c.addi4spn: reserved";
    if (simm % 4 != 0) return "Immediate must be a multiple of 4";

    cd = d - 8;

    asm_emit_16(C_ADDI4SPN(cd, simm), p->startline);
    return NULL;
}

const char *handle_c_slli(Parser *p, const char *opcode, size_t opcode_len) {
    Reg d;
    i32 simm;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    if (d == 0) return "rd cannot be x0 for c.slli: reserved";

    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if (!parse_numeric(p, &simm)) return "Invalid immediate";
    if (simm < 0 || simm >= 32) return "Invalid shift immediate";
    if (simm == 0) return "Shift immediate cannot be 0 for c.slli: reserved";

    asm_emit_16(C_SLLI(d, simm), p->startline);
    return NULL;
}

const char *handle_c_shift_right(Parser *p, const char *opcode,
                                 size_t opcode_len) {
    Reg d;
    SmallReg cd;
    i32 simm;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    if (d < 8 || d > 15) return "rd must be x8..x15";

    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if (!parse_numeric(p, &simm)) return "Invalid immediate";
    if (simm < 0 || simm >= 32) return "Invalid shift immediate";

    cd = d - 8;

    if (str_eq_case(opcode, opcode_len, "c.srli")) {
        if (simm == 0) return "shamt can't be 0 for c.srli: HINT";
        asm_emit_16(C_SRLI(cd, simm), p->startline);
    } else if (str_eq_case(opcode, opcode_len, "c.srai")) {
        if (simm == 0) return "shamt can't be 0 for c.srai: HINT";
        asm_emit_16(C_SRAI(cd, simm), p->startline);
    }

    return NULL;
}

const char *handle_c_andi(Parser *p, const char *opcode, size_t opcode_len) {
    Reg d;
    SmallReg cd;
    i32 simm;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    if (d < 8 || d > 15) return "rd must be x8..x15 for c.andi";

    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if (!parse_numeric(p, &simm)) return "Invalid immediate";
    if (simm < -32 || simm > 31) return "Out of bounds immediate";

    cd = d - 8;

    asm_emit_16(C_ANDI(cd, simm), p->startline);
    return NULL;
}

const char *handle_c_mv_add(Parser *p, const char *opcode, size_t opcode_len) {
    Reg rd, rs2;

    skip_trailing(p);
    if ((rd = parse_reg(p)) == (Reg)-1) return "Invalid rd";
    if (rd == 0) return "rd cannot be x0 for c.mv or c.add";

    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if ((rs2 = parse_reg(p)) == (Reg)-1) return "Invalid rs2";
    if (rs2 == 0) return "rs2 cannot be x0";

    if (str_eq_case(opcode, opcode_len, "c.mv")) {
        asm_emit_16(C_MV(rd, rs2), p->startline);
    } else if (str_eq_case(opcode, opcode_len, "c.add")) {
        asm_emit_16(C_ADD(rd, rs2), p->startline);
    }

    return NULL;
}

const char *handle_c_logic(Parser *p, const char *opcode, size_t opcode_len) {
    Reg d, s2;
    SmallReg cd, cs2;

    skip_trailing(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    if (d < 8 || d > 15) return "rd must be x8..x15";

    skip_trailing(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_trailing(p);
    if ((s2 = parse_reg(p)) == -1) return "Invalid rs2";
    if (s2 < 8 || s2 > 15) return "rs2 must be x8..x15";

    cd = d - 8;
    cs2 = s2 - 8;

    if (str_eq_case(opcode, opcode_len, "c.and")) {
        asm_emit_16(C_AND(cd, cs2), p->startline);
    } else if (str_eq_case(opcode, opcode_len, "c.or")) {
        asm_emit_16(C_OR(cd, cs2), p->startline);
    } else if (str_eq_case(opcode, opcode_len, "c.xor")) {
        asm_emit_16(C_XOR(cd, cs2), p->startline);
    } else if (str_eq_case(opcode, opcode_len, "c.sub")) {
        asm_emit_16(C_SUB(cd, cs2), p->startline);
    }

    return NULL;
}

const char *handle_c_nop(Parser *p, const char *opcode, size_t opcode_len) {
    i32 value = 0;
    if (peek(p) >= '0' && peek(p) <= '9') {
        if (!parse_numeric(p, &value)) {
            return "Invalid nop hint value";
        }
    }
    asm_emit_16(C_ADDI(0, value), p->startline);
    return NULL;
}

const char *handle_c_ebreak(Parser *p, const char *opcode, size_t opcode_len) {
    asm_emit_16(C_EBREAK(), p->startline);
    return NULL;
}

typedef struct OpcodeHandling {
    DeferredInsnCb *cb;
    const char *opcodes[64];
} OpcodeHandling;

OpcodeHandling opcode_types[] = {
    {
        handle_alu_reg,
        {"add", "slt", "sltu", "and", "or", "xor", "sll", "srl", "sub", "sra",
         "mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"},
    },
    {handle_alu_imm,
     {"addi", "slti", "sltiu", "andi", "ori", "xori", "slli", "srli", "srai"}},
    {handle_ext, {"sext.b", "sext.h", "zext.b", "zext.h"}},
    {handle_ldst, {"lb", "lh", "lw", "lbu", "lhu", "sb", "sh", "sw"}},
    {handle_branch,
     {"beq", "bne", "blt", "bge", "bltu", "bgeu", "bgt", "ble", "bgtu",
      "bleu"}},
    {handle_branch_zero, {"beqz", "bnez", "blez", "bgez", "bltz", "bgtz"}},
    {handle_alu_pseudo, {"mv", "not", "neg", "seqz", "snez", "sltz", "sgtz"}},
    {handle_jump, {"j", "jal", "call"}},
    {handle_jump_reg, {"jr", "jalr"}},
    {handle_ret, {"ret"}},
    {handle_upper, {"lui", "auipc"}},
    {handle_li, {"li"}},
    {handle_la, {"la"}},
    {handle_ecall, {"ecall"}},
    {handle_ebreak, {"ebreak"}},
    {handle_nop, {"nop"}},
    {handle_csr, {"csrrw", "csrrs", "csrrc"}},
    {handle_csr_imm, {"csrrwi", "csrrsi", "csrrci"}},
    {handle_sret, {"sret"}},
    // compressed instructions
    {handle_c_lwsp, {"c.lwsp"}},
    {handle_c_swsp, {"c.swsp"}},
    {handle_c_lw, {"c.lw"}},
    {handle_c_sw, {"c.sw"}},
    {handle_c_jump, {"c.j", "c.jal"}},
    {handle_c_jump_reg, {"c.jr", "c.jalr"}},
    {handle_c_branch_zero, {"c.beqz", "c.bnez"}},
    {handle_c_li, {"c.li"}},
    {handle_c_lui, {"c.lui"}},
    {handle_c_addi, {"c.addi"}},
    {handle_c_addi16sp, {"c.addi16sp"}},
    {handle_c_addi4spn, {"c.addi4spn"}},
    {handle_c_slli, {"c.slli"}},
    {handle_c_shift_right, {"c.srli", "c.srai"}},
    {handle_c_andi, {"c.andi"}},
    {handle_c_mv_add, {"c.mv", "c.add"}},
    {handle_c_logic, {"c.and", "c.or", "c.xor", "c.sub"}},
    {handle_c_nop, {"c.nop"}},
    {handle_c_ebreak, {"c.ebreak"}},
};

// defining _start but not making it global is a VERY common mistake
// another mistake i've seen is putting _start in .data by accident
const char *resolve_start(u32 *start_pc) {
    Section *section;
    if (!resolve_symbol("_start", strlen("_start"), true, start_pc, &section)) {
        if (resolve_symbol("_start", strlen("_start"), false, start_pc,
                           &section)) {
            return "_start defined, but without .globl";
        }
        // if it's not defined and not global, then there is no _start at all
        // just assign it to the default
        *start_pc = TEXT_BASE;
        return NULL;
    }
    if (section != g_text) {
        return "_start not in .text section";
    }
    return NULL;
}

const char *resolve_kernel_start(u32 *start_pc) {
    Section *section;
    if (!resolve_symbol("_kernel_start", strlen("_kernel_start"), true,
                        start_pc, &section)) {
        if (resolve_symbol("_kernel_start", strlen("_kernel_start"), false,
                           start_pc, &section)) {
            return "_kernel_start defined, but without .globl";
        }

        return "_kernel_start symbol not found";
    }

    if (section != g_kernel_text) {
        return "_kernel_start not in .kernel_text section";
    }

    return NULL;
}
const char *resolve_entry(u32 *start_pc) {
    if (resolve_kernel_start(start_pc) == NULL) {
        emulator_enter_kernel();
        return NULL;
    }

    return resolve_start(start_pc);
}

static void prepare_default_syms(void) {
#define MMIO_LABEL(name, addrr)                                    \
    *ARES_ARRAY_PUSH(&g_labels) = (LabelData){.txt = (name),       \
                                              .len = strlen(name), \
                                              .addr = (addrr),     \
                                              .section = g_mmio}

    MMIO_LABEL("_MMIO_BASE", MMIO_BASE);
    MMIO_LABEL("_MMIO_END", MMIO_END);

    MMIO_LABEL("_DMA0_BASE", DMA0_BASE);
    MMIO_LABEL("_DMA0_DST_ADDR", DMA0_DST_ADDR);
    MMIO_LABEL("_DMA0_SRC_ADDR", DMA0_SRC_ADDR);
    MMIO_LABEL("_DMA0_DST_INC", DMA0_DST_INC);
    MMIO_LABEL("_DMA0_SRC_INC", DMA0_SRC_INC);
    MMIO_LABEL("_DMA0_LEN", DMA0_LEN);
    MMIO_LABEL("_DMA0_TRANS_SIZE", DMA0_TRANS_SIZE);
    MMIO_LABEL("_DMA0_CNTL", DMA0_CNTL);
    MMIO_LABEL("_DMA0_END", DMA0_END);

    MMIO_LABEL("_DMA1_BASE", DMA1_BASE);
    MMIO_LABEL("_DMA1_DST_ADDR", DMA1_DST_ADDR);
    MMIO_LABEL("_DMA1_SRC_ADDR", DMA1_SRC_ADDR);
    MMIO_LABEL("_DMA1_DST_INC", DMA1_DST_INC);
    MMIO_LABEL("_DMA1_SRC_INC", DMA1_SRC_INC);
    MMIO_LABEL("_DMA1_LEN", DMA1_LEN);
    MMIO_LABEL("_DMA1_TRANS_SIZE", DMA1_TRANS_SIZE);
    MMIO_LABEL("_DMA1_CNTL", DMA1_CNTL);
    MMIO_LABEL("_DMA1_END", DMA1_END);

    MMIO_LABEL("_DMA2_BASE", DMA2_BASE);
    MMIO_LABEL("_DMA2_DST_ADDR", DMA2_DST_ADDR);
    MMIO_LABEL("_DMA2_SRC_ADDR", DMA2_SRC_ADDR);
    MMIO_LABEL("_DMA2_DST_INC", DMA2_DST_INC);
    MMIO_LABEL("_DMA2_SRC_INC", DMA2_SRC_INC);
    MMIO_LABEL("_DMA2_LEN", DMA2_LEN);
    MMIO_LABEL("_DMA2_TRANS_SIZE", DMA2_TRANS_SIZE);
    MMIO_LABEL("_DMA2_CNTL", DMA2_CNTL);
    MMIO_LABEL("_DMA2_END", DMA2_END);

    MMIO_LABEL("_DMA3_BASE", DMA3_BASE);
    MMIO_LABEL("_DMA3_DST_ADDR", DMA3_DST_ADDR);
    MMIO_LABEL("_DMA3_SRC_ADDR", DMA3_SRC_ADDR);
    MMIO_LABEL("_DMA3_DST_INC", DMA3_DST_INC);
    MMIO_LABEL("_DMA3_SRC_INC", DMA3_SRC_INC);
    MMIO_LABEL("_DMA3_LEN", DMA3_LEN);
    MMIO_LABEL("_DMA3_TRANS_SIZE", DMA3_TRANS_SIZE);
    MMIO_LABEL("_DMA3_CNTL", DMA3_CNTL);
    MMIO_LABEL("_DMA3_END", DMA3_END);

    MMIO_LABEL("_POWER0_BASE", POWER0_BASE);
    MMIO_LABEL("_POWER0_CNTL", POWER0_CNTL);
    MMIO_LABEL("_POWER0_END", POWER0_END);

    MMIO_LABEL("_CONSOLE0_BASE", CONSOLE0_BASE);
    MMIO_LABEL("_CONSOLE0_IN", CONSOLE0_IN);
    MMIO_LABEL("_CONSOLE0_OUT", CONSOLE0_OUT);
    MMIO_LABEL("_CONSOLE0_IN_SIZE", CONSOLE0_IN_SIZE);
    MMIO_LABEL("_CONSOLE0_BATCH_SIZE", CONSOLE0_BATCH_SIZE);
    MMIO_LABEL("_CONSOLE0_CNTL", CONSOLE0_CNTL);
    MMIO_LABEL("_CONSOLE0_END", CONSOLE0_END);

    MMIO_LABEL("_RIC0_BASE", RIC0_BASE);
    MMIO_LABEL("_RIC0_DEVADDR", RIC0_DEVADDR);
    MMIO_LABEL("_RIC0_END", RIC0_END);

#undef MMIO_LABEL
}

// .word can contain labels, and labels may come later in the text
// and also, .word can get an array
// so, to simplify the deferred instruction code
// i make so that a word is always filled in the fixup phase
// where all the label positions are certain
const char *parse_word(Parser *p, const char *opcode, size_t opcode_len) {
    Parser orig = *p;

    if (!g_in_fixup) {
        DeferredInsn *insn = ARES_ARRAY_PUSH(&g_deferred_insn);
        insn->emit_idx = g_section->emit_idx;
        insn->p = orig;
        insn->cb = parse_word;
        insn->opcode = opcode;
        insn->opcode_len = opcode_len;
        insn->section = g_section;

        bool first = true;
        while (true) {
            skip_whitespace(p);
            if (!first && !consume_if(p, ',')) break;
            first = false;
            skip_whitespace(p);

            i32 dummy;
            if (!parse_numeric(p, &dummy)) {
                const char *tok;
                size_t tok_len;
                if (!parse_ident(p, &tok, &tok_len) || tok_len == 0)
                    return "Invalid word";
            }
            asm_emit(0, p->startline);
        }
        return NULL;
    }

    bool first = true;
    while (true) {
        skip_whitespace(p);
        if (!first && !consume_if(p, ',')) break;
        first = false;
        skip_whitespace(p);

        i32 value;
        if (parse_numeric(p, &value)) {
            asm_emit(value, p->startline);
        } else {
            bool later;
            u32 addr = 0;
            const char *err = label(p, &orig, parse_word, opcode, opcode_len,
                                    &addr, &later, reloc_abs32);
            if (err) return "Invalid word";
            asm_emit((i32)addr, p->startline);
        }
    }
    return NULL;
}

/*
    NOTE: both binutils and RARS do not support instructions spanning more
   lines, like "li a0,\n93" But RARS does support it for .byte etc, so for
   compatibility we do it too
*/

export void assemble(const char *txt, size_t s, bool allow_externs) {
    g_allow_externs = allow_externs;
    g_in_fixup = false;

    callsan_init();
    emulator_init();

    g_text = malloc(sizeof(*g_text));
    ares_panic_if_null(g_text);
    g_data = malloc(sizeof(*g_data));
    ares_panic_if_null(g_data);
    g_kernel_data = malloc(sizeof(*g_kernel_data));
    ares_panic_if_null(g_kernel_data);
    g_kernel_text = malloc(sizeof(*g_kernel_text));
    ares_panic_if_null(g_kernel_text);

    *g_text = (Section){.name = ".text",
                        .base = TEXT_BASE,
                        .limit = TEXT_END,
                        .contents = ARES_ARRAY_NEW(u8),
                        .emit_idx = 0,
                        .align = 4,
                        .relocations = ARES_ARRAY_NEW(Relocation),
                        .read = true,
                        .write = false,
                        .execute = true,
                        .super = false,
                        .physical = true};

    *g_data = (Section){.name = ".data",
                        .base = DATA_BASE,
                        .limit = DATA_END,
                        .contents = ARES_ARRAY_NEW(u8),
                        .emit_idx = 0,
                        .align = 1,
                        .relocations = ARES_ARRAY_NEW(Relocation),
                        .read = true,
                        .write = true,
                        .execute = false,
                        .super = false,
                        .physical = true};

    *g_kernel_data = (Section){.name = ".kernel_data",
                               .base = KERNEL_DATA_BASE,
                               .limit = KERNEL_DATA_END,
                               .contents = ARES_ARRAY_NEW(u8),
                               .emit_idx = 0,
                               .align = 1,
                               .relocations = ARES_ARRAY_NEW(Relocation),
                               .read = true,
                               .write = true,
                               .execute = false,
                               .super = true,
                               .physical = false};

    *g_kernel_text = (Section){.name = ".kernel_text",
                               .base = KERNEL_TEXT_BASE,
                               .limit = KERNEL_TEXT_END,
                               .contents = ARES_ARRAY_NEW(u8),
                               .emit_idx = 0,
                               .align = 1,
                               .relocations = ARES_ARRAY_NEW(Relocation),
                               .read = true,
                               .write = false,
                               .execute = true,
                               .super = true,
                               .physical = false};

    prepare_runtime_sections();
    prepare_default_syms();
    g_section = g_text;

    Parser parser = {0};
    parser.input = txt;
    parser.size = s;
    parser.pos = 0;
    parser.lineidx = 1;
    Parser *p = &parser;
    const char *err = NULL;

    while (!err) {
        skip_whitespace(p);
        if (p->pos == p->size) break;
        p->startline = p->lineidx;

        // i can fail parsing sections
        // if so, the identifier starting with . is a temp label
        // yes, this sucks
        Parser old = *p;
        if (consume_if(p, '.')) {
            const char *directive;
            size_t directive_len;
            if (!parse_ident(p, &directive, &directive_len)) {
                err = "Invalid directive";
                break;
            }
            skip_trailing(p);

            if (str_eq_case(directive, directive_len, "section")) {
                const char *secname;
                size_t secname_len;
                parse_ident(p, &secname, &secname_len);
                SectionPtr sec = NULL;
                // scan already-existing section names
                for (size_t i = 0; !sec && i < g_sections.len; i++)
                    if (str_eq(secname, secname_len, g_sections.buf[i]->name))
                        sec = g_sections.buf[i];
                if (!sec) {
                    err = "Section not found";
                    break;
                }
                g_section = sec;
                continue;
            }

            if (str_eq_case(directive, directive_len, "data")) {
                g_section = g_data;
                continue;
            } else if (str_eq_case(directive, directive_len, "text")) {
                g_section = g_text;
                continue;
            } else if (str_eq_case(directive, directive_len, "globl")) {
                skip_trailing(p);
                const char *ident;
                size_t ident_len;
                if (!parse_ident(p, &ident, &ident_len) || ident_len == 0) {
                    err = "Expected identifier after .globl";
                    break;
                }
                *ARES_ARRAY_PUSH(&g_globals) =
                    (Global){.str = ident, .len = ident_len};
                continue;
            } else if (str_eq_case(directive, directive_len, "byte")) {
                i32 value;
                bool first = true;
                while (true) {
                    skip_whitespace(p);
                    if (first || consume_if(p, ',')) {
                        skip_whitespace(p);
                        if (!parse_numeric(p, &value)) {
                            err = "Invalid byte";
                            break;
                        }
                        if (value < -128 || value > 255) {
                            err = "Out of bounds byte";
                            break;
                        }
                        asm_emit_byte(value, p->startline);
                    } else break;
                    first = false;
                }
                continue;
            } else if (str_eq_case(directive, directive_len, "half")) {
                i32 value;
                bool first = true;
                while (true) {
                    skip_whitespace(p);
                    if (first || consume_if(p, ',')) {
                        skip_whitespace(p);
                        if (!parse_numeric(p, &value)) {
                            err = "Invalid half";
                            break;
                        }
                        if (value < -32768 || value > 65535) {
                            err = "Out of bounds half";
                            break;
                        }
                        asm_emit_byte(value, p->startline);
                        asm_emit_byte(value >> 8, p->startline);
                    } else break;
                    first = false;
                }
                continue;
            } else if (str_eq_case(directive, directive_len, "word")) {
                err = parse_word(p, "word", 4);
                if (err) break;
                continue;
            } else if (str_eq_case(directive, directive_len, "ascii")) {
                char *out;
                size_t out_len;
                bool first = true;
                while (true) {
                    skip_whitespace(p);
                    if (first || consume_if(p, ',')) {
                        skip_whitespace(p);
                        if (!parse_quoted_str(p, &out, &out_len)) {
                            err = "Invalid string";
                            break;
                        }
                        for (size_t i = 0; i < out_len; i++)
                            asm_emit_byte(out[i], p->startline);
                        free(out);
                    } else break;
                    first = false;
                }
                continue;
            } else if (str_eq_case(directive, directive_len, "asciz") ||
                       str_eq_case(directive, directive_len, "asciiz") ||
                       str_eq_case(directive, directive_len, "string")) {
                char *out;
                size_t out_len;
                bool first = true;
                while (true) {
                    skip_whitespace(p);
                    if (first || consume_if(p, ',')) {
                        skip_whitespace(p);
                        if (!parse_quoted_str(p, &out, &out_len)) {
                            err = "Invalid string";
                            break;
                        }
                        for (size_t i = 0; i < out_len; i++)
                            asm_emit_byte(out[i], p->startline);
                        asm_emit_byte(0, p->startline);
                        free(out);
                    } else break;
                    first = false;
                }
                continue;
            } else {
                // backtrack if not a valid directive
                // it means that it's a label
                // so stuff like .inner_label: is valid
                *p = old;
            }
        }

        const char *ident, *opcode;
        size_t ident_len, opcode_len;
        parse_ident(p, &ident, &ident_len);
        // IMPORTANT: it needs to be skip trailing here
        // otherwise, it will happily consume the newline after
        // no-param instructions, like ret and nop
        skip_trailing(p);

        if (ident_len > 0 && consume_if(p, ':')) {
            for (size_t i = 0; i < ARES_ARRAY_LEN(&g_labels); i++) {
                if (str_eq_2(ARES_ARRAY_GET(&g_labels, i)->txt,
                             ARES_ARRAY_GET(&g_labels, i)->len, ident,
                             ident_len)) {
                    err = "Multiple definitions for the same label";
                    break;
                }
            }
            u32 addr = g_section->emit_idx + g_section->base;
            *ARES_ARRAY_PUSH(&g_labels) = (LabelData){.txt = ident,
                                                      .len = ident_len,
                                                      .addr = addr,
                                                      .section = g_section};
            continue;
        }

        opcode = ident;
        opcode_len = ident_len;

        bool found = false;
        for (size_t i = 0;
             !found && i < sizeof(opcode_types) / sizeof(OpcodeHandling); i++) {
            for (size_t j = 0; !found && opcode_types[i].opcodes[j]; j++) {
                if (str_eq_case(opcode, opcode_len,
                                opcode_types[i].opcodes[j])) {
                    found = true;
                    err = opcode_types[i].cb(p, opcode, opcode_len);
                }
            }
        }
        if (!found) {
            err = "Unknown opcode";
        }
        if (err) break;

        skip_trailing(p);
        char next = peek(p);
        if (next != '\n' && next != '\0') {
            err = "Expected newline";
            break;
        }
    }

    if (!err) {
        g_in_fixup = true;
        for (size_t i = 0; i < ARES_ARRAY_LEN(&g_deferred_insn); i++) {
            struct DeferredInsn *insn = ARES_ARRAY_GET(&g_deferred_insn, i);
            g_section = insn->section;
            g_section->emit_idx = insn->emit_idx;
            p = &insn->p;
            err = insn->cb(&insn->p, insn->opcode, insn->opcode_len);
            if (err) break;
        }
    }

    if (err) {
        g_error = err;
        g_error_line = p->startline;
        return;
    }

    err = resolve_entry(&g_pc);
    if (err) {
        g_error = err;
        g_error_line = 1;
    }
}

bool pc_to_label_r(u32 pc, LabelData **ret, u32 *off) {
    LabelData *closest = NULL;

    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_labels); i++) {
        if (ARES_ARRAY_GET(&g_labels, i)->addr <= pc &&
            (!closest || ARES_ARRAY_GET(&g_labels, i)->addr > closest->addr)) {
            closest = ARES_ARRAY_GET(&g_labels, i);
        }
    }

    if (closest) {
        *ret = closest;
        *off = pc - closest->addr;
        return true;
    }

    *ret = NULL;
    *off = 0;
    return false;
}

export u32 g_get_addr_from_line_start;
export u32 g_get_addr_from_line_end;

void get_addr_from_line(u32 line) {
    Section *startsec = NULL;
    size_t j = 0;

    // find the section containing the line
    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_sections); i++) {
        startsec = g_sections.buf[i];
        for (j = 0; j < ARES_ARRAY_LEN(&startsec->by_linenum); j++) {
            if (startsec->by_linenum.buf[j] == line) {
                g_get_addr_from_line_start = startsec->base + j;
                goto found_line;
            }
        }
    }

    // line not found in any section
    g_get_addr_from_line_start = 0;
    g_get_addr_from_line_end = 0;
    return;

found_line:
    // is there an instruction after this?
    for (; j < ARES_ARRAY_LEN(&startsec->by_linenum); j++) {
        if (startsec->by_linenum.buf[j] > line) {
            g_get_addr_from_line_end = startsec->base + j;
            return;
        }
    }
    // if instead it was the last line, the end is the section end
    g_get_addr_from_line_end = startsec->base + startsec->contents.len;
}

u32 get_line_from_pc() {
    if (g_pc < TEXT_BASE) return 0;
    size_t idx = g_pc - TEXT_BASE;
    if (idx >= g_text->by_linenum.len) return 0;
    return g_text->by_linenum.buf[idx];
}

// Ugly because i'm calling it from JS
// The problem with this is that it's basically the cleanest way to do it
const char *g_pc_to_label_txt;
size_t g_pc_to_label_len;
u32 g_pc_to_label_off;
void pc_to_label(u32 pc) {
    LabelData *l;
    if (pc_to_label_r(pc, &l, &g_pc_to_label_off)) {
        g_pc_to_label_txt = l->txt;
        g_pc_to_label_len = l->len;
        return;
    }
    g_pc_to_label_txt = NULL;
    g_pc_to_label_len = 0;
}

bool resolve_symbol(const char *sym, size_t sym_len, bool global, u32 *addr,
                    Section **sec) {
    LabelData *ret = NULL;
    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_labels); i++) {
        LabelData *l = ARES_ARRAY_GET(&g_labels, i);
        if (str_eq_2(sym, sym_len, l->txt, l->len)) {
            ret = l;
            break;
        }
    }
    if (ret && global) {
        for (size_t i = 0; i < ARES_ARRAY_LEN(&g_globals); i++)
            if (str_eq_2(sym, sym_len, ARES_ARRAY_GET(&g_globals, i)->str,
                         ARES_ARRAY_GET(&g_globals, i)->len)) {
                *addr = ret->addr;
                if (sec) {
                    *sec = ret->section;
                }
                return true;
            }
        return false;
    }
    if (ret) {
        *addr = ret->addr;
        if (sec) {
            *sec = ret->section;
        }
        return true;
    }
    return false;
}

void prepare_aux_sections(void) {
    g_stack = malloc(sizeof(Section));
    ares_panic_if_null(g_stack);
    *g_stack = (Section){.name = "ARES_STACK",
                         .base = STACK_TOP - STACK_LEN,
                         .limit = STACK_TOP,
                         .contents = ARES_ARRAY_FILL(u8, STACK_LEN),
                         .emit_idx = 0,
                         .align = 1,
                         .relocations = {.buf = NULL, .len = 0, .cap = 0},
                         .read = true,
                         .write = true,
                         .execute = false,
                         .physical = false};
    // fill all the memory with random uninitialized values
    memset(g_stack->contents.buf, 0xAB, g_stack->contents.len);

    g_regs[2] = STACK_TOP;  // FIXME: now i am diverging from RARS, which
                            // does STACK_TOP - 4

    g_mmio = malloc(sizeof(*g_mmio));
    ares_panic_if_null(g_mmio);
    *g_mmio = (Section){.name = ".mmio",
                        .base = MMIO_BASE,
                        .limit = MMIO_END,
                        .contents = ARES_ARRAY_NEW(u8),
                        .emit_idx = 0,
                        .align = 1,
                        .relocations = ARES_ARRAY_NEW(Relocation),
                        .read = true,
                        .write = true,
                        .execute = false,
                        .super = true,
                        .physical = false};

    *ARES_ARRAY_PUSH(&g_sections) = g_stack;
    *ARES_ARRAY_PUSH(&g_sections) = g_mmio;
}

void prepare_runtime_sections(void) {
    // TODO: dynamically growing stacks?

    *ARES_ARRAY_PUSH(&g_sections) = g_text;
    *ARES_ARRAY_PUSH(&g_sections) = g_data;
    *ARES_ARRAY_PUSH(&g_sections) = g_kernel_text;
    *ARES_ARRAY_PUSH(&g_sections) = g_kernel_data;
}

void free_runtime(void) {
    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_sections); i++) {
        Section *s = *ARES_ARRAY_GET(&g_sections, i);
        ARES_ARRAY_FREE(&s->relocations);
        ARES_ARRAY_FREE(&s->contents);
        ARES_ARRAY_FREE(&s->by_linenum);
        free(s);
    }

    ARES_ARRAY_FREE(&g_sections);
    ARES_ARRAY_FREE(&g_labels);
    ARES_ARRAY_FREE(&g_deferred_insn);
    ARES_ARRAY_FREE(&g_globals);
    ARES_ARRAY_FREE(&g_externs);
    ARES_ARRAY_FREE(&g_pcrel_hi_relocs);
    ARES_ARRAY_FREE(&g_shadow_stack);
}

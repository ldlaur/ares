#include "ares/core.h"

#include <stddef.h>

#include "ares/callsan.h"
#include "ares/dev.h"
#include "ares/elf.h"
#include "ares/emulate.h"

export Section *g_text, *g_data, *g_stack, *g_kernel_text, *g_kernel_data,
    *g_mmio;

ARES_ARRAY(SectionPtr) g_sections = ARES_ARRAY_NEW(SectionPtr);
ARES_ARRAY(Extern) g_externs = ARES_ARRAY_NEW(Extern);
ARES_ARRAY(LabelData) g_labels = ARES_ARRAY_NEW(LabelData);
ARES_ARRAY(Global) g_globals = ARES_ARRAY_NEW(Global);
export ARES_ARRAY(u32) g_text_by_linenum;

static ARES_ARRAY(DeferredInsn)
    g_deferred_insn = ARES_ARRAY_NEW(DeferredInsn);

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
InstI(CSRRW, 0x1C, 0b001);
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
InstA(MULU,  0b01100, 0b010, 0, 1)
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

bool whitespace(char c) {
    return c == '\n' || c == '\t' || c == ' ' || c == '\r';
}
bool trailing(char c) { return c == '\t' || c == ' '; }

bool digit(char c) { return (c >= '0' && c <= '9'); }
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

void parse_ident(Parser *p, const char **str, size_t *len) {
    size_t start = p->pos;
    while (p->pos < p->size && ident(p->input[p->pos])) advance(p);
    size_t end = p->pos;
    *str = p->input + start;
    *len = end - start;
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
    return memcmp(s1, s2, s1len) == 0;
}

bool parse_numeric(Parser *p, i32 *out) {
    Parser start = *p;
    bool negative = false;
    bool parsed_digit = false;
    u32 value = 0;  // u32 to avoid the issue of signed overflow
    int base = 10;
    while (peek(p) == '-' || peek(p) == '+') {
        if (consume_if(p, '-')) negative = !negative;
        consume_if(p, '+');
    }

    if (consume_if(p, '\'')) {
        char c;
        if (!consume(p, &c)) return false;
        if (c == '\\') {
            if (!consume(p, &c)) return false;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            else if (c == 'b') c = '\b';
            else if (c == 'f') c = '\f';
            else if (c == 'a') c = '\a';
            else if (c == 'b') c = '\b';
            else if (c == '\\') c = '\\';
            else if (c == '\'') c = '\'';
            else if (c == '"') c = '"';
            else if (c == '0') c = 0;
            else return false;
        }
        value = (unsigned char)c;
        if (!consume_if(p, '\'')) return false;
    } else {
        if (peek(p) == '0') {
            char prefix = peek_n(p, 1);
            if (prefix == 'x' || prefix == 'X') base = 16;
            else if (prefix == 'b' || prefix == 'B') base = 2;
            if (base != 10) advance_n(p, 2);
        }

        // TODO: handle overflow
        for (char c; (c = peek(p));) {
            int digit = base;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            if (digit >= base) {
                if (whitespace(c)) break;
                if (c == ' ' || c == '(' || c == ',' || c == '\0') break;
                return false;
            }
            parsed_digit = true;
            value = value * base + digit;
            advance(p);
        }

        if (!parsed_digit) {
            *p = start;
            return false;
        }
    }
    if (negative) value = -value;
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
            else if (c == 'b') c = '\b';
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
        if (len == 2) return str[1] - '0';
        else {
            int num = (str[1] - '0') * 10 + (str[2] - '0');
            if (num >= 32) return -1;
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
    const char *str;
    size_t len;
    parse_ident(p, &str, &len);

    for (int i = 0; i < sizeof(CSR_NAMES)/sizeof(CSR_NAMES[0]); i++) {
        if (CSR_NAMES[i] && str_eq_case(str, len, CSR_NAMES[i])) return i;
    }

    return -1;
}

void asm_emit_byte(u8 byte, int linenum) {
    if (!g_in_fixup) {
        *ARES_ARRAY_PUSH(&g_section->contents) = byte;
    } else {
        *ARES_ARRAY_INSERT(&g_section->contents, g_section->emit_idx) = byte;
    }
    g_section->emit_idx++;
}

void asm_emit(u32 inst, int linenum) {
    if (g_section == g_text) {
        *ARES_ARRAY_PUSH(&g_text_by_linenum) = linenum;
    }

    asm_emit_byte(inst >> 0, linenum);
    asm_emit_byte(inst >> 8, linenum);
    asm_emit_byte(inst >> 16, linenum);
    asm_emit_byte(inst >> 24, linenum);
}

static Extern *get_extern(const char *sym, size_t sym_len) {
    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_externs); i++) {
        if (ARES_ARRAY_GET(&g_externs, i)->len == sym_len &&
            0 ==
                memcmp(sym, ARES_ARRAY_GET(&g_externs, i)->symbol, sym_len)) {
            return ARES_ARRAY_GET(&g_externs, i);
        }
    }

    Extern *e = ARES_ARRAY_PUSH(&g_externs);
    e->symbol = sym;
    e->len = sym_len;
    return e;
}

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

const char *handle_alu_reg(Parser *p, const char *opcode, size_t opcode_len) {
    int d, s1, s2;

    skip_whitespace(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    skip_whitespace(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_whitespace(p);
    if ((s1 = parse_reg(p)) == -1) return "Invalid rs1";
    skip_whitespace(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_whitespace(p);
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
    else if (str_eq_case(opcode, opcode_len, "mulu")) inst = MULU(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "mulhu")) inst = MULHU(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "div")) inst = DIV(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "divu")) inst = DIVU(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "rem")) inst = REM(d, s1, s2);
    else if (str_eq_case(opcode, opcode_len, "remu")) inst = REMU(d, s1, s2);

    asm_emit(inst, p->startline);
    return NULL;
}

const char *handle_alu_imm(Parser *p, const char *opcode, size_t opcode_len) {
    int d, s1;
    i32 simm;

    skip_whitespace(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    skip_whitespace(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_whitespace(p);
    if ((s1 = parse_reg(p)) == -1) return "Invalid rs1";
    skip_whitespace(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_whitespace(p);

    if (!parse_numeric(p, &simm)) return "Invalid imm";
    if (simm < -2048 || simm > 2047) return "Out of bounds imm";

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
    int reg, mem;
    i32 simm;

    skip_whitespace(p);
    if ((reg = parse_reg(p)) == -1) return "Invalid rreg";
    skip_whitespace(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_whitespace(p);
    if (!parse_numeric(p, &simm)) return "Invalid imm";
    if (simm < -2048 || simm > 2047) return "Out of bounds imm";

    skip_whitespace(p);
    if (!consume_if(p, '(')) return "Expected (";
    skip_whitespace(p);
    if ((mem = parse_reg(p)) == -1) return "Invalid rmem";
    skip_whitespace(p);
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
    insn->reloc = reloc;
    insn->opcode = opcode;
    insn->opcode_len = opcode_len;
    insn->section = g_section;
    *later = true;
    return NULL;
}

const char *handle_branch(Parser *p, const char *opcode, size_t opcode_len) {
    Parser orig = *p;
    u32 addr;
    int s1, s2;
    bool later;

    skip_whitespace(p);
    if ((s1 = parse_reg(p)) == -1) return "Invalid rs1";
    skip_whitespace(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_whitespace(p);
    if ((s2 = parse_reg(p)) == -1) return "Invalid rs2";
    skip_whitespace(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_whitespace(p);
    const char *err = label(p, &orig, handle_branch, opcode, opcode_len, &addr,
                            &later, reloc_branch);
    if (err) return err;
    if (later) {
        asm_emit(0, p->startline);
        return NULL;
    }
    i32 simm = addr - (g_section->emit_idx + g_section->base);

    u32 inst = 0;
    if (str_eq_case(opcode, opcode_len, "beq")) inst = BEQ(s1, s2, simm);
    else if (str_eq_case(opcode, opcode_len, "bne")) inst = BNE(s1, s2, simm);
    else if (str_eq_case(opcode, opcode_len, "blt")) inst = BLT(s1, s2, simm);
    else if (str_eq_case(opcode, opcode_len, "bge")) inst = BGE(s1, s2, simm);
    else if (str_eq_case(opcode, opcode_len, "bltu")) inst = BLTU(s1, s2, simm);
    else if (str_eq_case(opcode, opcode_len, "bgeu")) inst = BGEU(s1, s2, simm);
    else if (str_eq_case(opcode, opcode_len, "bgt")) inst = BLT(s2, s1, simm);
    else if (str_eq_case(opcode, opcode_len, "ble")) inst = BGE(s2, s1, simm);
    else if (str_eq_case(opcode, opcode_len, "bgtu")) inst = BLTU(s2, s1, simm);
    else if (str_eq_case(opcode, opcode_len, "bleu")) inst = BGEU(s2, s1, simm);
    asm_emit(inst, p->startline);
    return NULL;
}

const char *handle_branch_zero(Parser *p, const char *opcode,
                               size_t opcode_len) {
    Parser orig = *p;
    u32 addr;
    int s;
    bool later;

    skip_whitespace(p);
    if ((s = parse_reg(p)) == -1) return "Invalid rs";
    skip_whitespace(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_whitespace(p);
    const char *err = label(p, &orig, handle_branch_zero, opcode, opcode_len,
                            &addr, &later, reloc_branch);
    if (err) return err;
    if (later) {
        asm_emit(0, p->startline);
        return NULL;
    }
    i32 simm = addr - (g_section->emit_idx + g_section->base);

    u32 inst = 0;
    if (str_eq_case(opcode, opcode_len, "beqz")) inst = BEQ(s, 0, simm);
    else if (str_eq_case(opcode, opcode_len, "bnez")) inst = BNE(s, 0, simm);
    else if (str_eq_case(opcode, opcode_len, "blez")) inst = BGE(0, s, simm);
    else if (str_eq_case(opcode, opcode_len, "bgez")) inst = BGE(s, 0, simm);
    else if (str_eq_case(opcode, opcode_len, "bltz")) inst = BLT(s, 0, simm);
    else if (str_eq_case(opcode, opcode_len, "bgtz")) inst = BLT(0, s, simm);

    asm_emit(inst, p->startline);
    return NULL;
}

const char *handle_alu_pseudo(Parser *p, const char *opcode,
                              size_t opcode_len) {
    u32 addr;
    int d, s;

    skip_whitespace(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    skip_whitespace(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_whitespace(p);
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

    skip_whitespace(p);
    // jal optionally takes a register argument
    if (str_eq_case(opcode, opcode_len, "jal")) {
        if ((d = parse_reg(p)) == -1) err = "Invalid rd";
        skip_whitespace(p);
        if (consume_if(p, ',')) {
            if (err) return err;
        } else {
            *p = orig;
            d = 1;
        }
    } else if (str_eq_case(opcode, opcode_len, "j")) {
        d = 0;
    } else assert(false);

    skip_whitespace(p);
    u32 addr;
    err = label(p, &orig, handle_jump, opcode, opcode_len, &addr, &later,
                reloc_jal);
    if (err) return err;
    if (later) {
        asm_emit(0, p->startline);
        return NULL;
    }
    i32 simm = addr - (g_section->emit_idx + g_section->base);
    asm_emit(JAL(d, simm), p->startline);
    return NULL;
}

const char *handle_jump_reg(Parser *p, const char *opcode, size_t opcode_len) {
    int d, s;
    i32 simm;

    skip_whitespace(p);
    // jalr rs
    // jalr rd, rs, simm
    // jalr rd, simm(rs)
    if (str_eq_case(opcode, opcode_len, "jalr")) {
        if ((d = parse_reg(p)) == -1) return "Invalid register";
        skip_whitespace(p);
        if (!consume_if(p, ',')) {
            asm_emit(JALR(1, d, 0), p->startline);
            return NULL;
        }
        skip_whitespace(p);
        if (parse_numeric(p, &simm)) {  // simm(rs)
            skip_whitespace(p);
            if (!consume_if(p, '(')) return "Expected (";
            skip_whitespace(p);
            if ((s = parse_reg(p)) == -1) return "Invalid rs";
            skip_whitespace(p);
            if (!consume_if(p, ')')) return "Expected )";
        } else if (consume_if(p, '(')) {  // (rs)
            simm = 0;
            skip_whitespace(p);
            if ((s = parse_reg(p)) == -1) return "Invalid rs";
            skip_whitespace(p);
            if (!consume_if(p, ')')) return "Expected )";
        } else {  // rs1, simm
            if ((s = parse_reg(p)) == -1) return "Invalid rs";
            skip_whitespace(p);
            if (!consume_if(p, ',')) return "Expected ,";
            skip_whitespace(p);
            if (!parse_numeric(p, &simm)) return "Invalid imm";
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
    int d;
    i32 simm;
    u32 inst = 0;

    skip_whitespace(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    skip_whitespace(p);
    if (!consume_if(p, ',')) return "Expected ,";
    skip_whitespace(p);
    if (!parse_numeric(p, &simm)) return "Invalid imm";
    // the immediate can either be signed or unsigned 20 bit
    if (simm < -524288 || simm > 1048575) return "Out of bounds imm";

    if (str_eq_case(opcode, opcode_len, "lui")) inst = LUI(d, simm);
    else if (str_eq_case(opcode, opcode_len, "auipc")) inst = AUIPC(d, simm);

    asm_emit(inst, p->startline);
    return NULL;
}

const char *handle_li(Parser *p, const char *opcode, size_t opcode_len) {
    int d;
    i32 simm;

    skip_whitespace(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    skip_whitespace(p);
    if (!consume_if(p, ',')) return "Expected ,";
    skip_whitespace(p);
    if (!parse_numeric(p, &simm)) return "Invalid imm";

    if (simm >= -2048 && simm <= 2047) {
        asm_emit(ADDI(d, 0, simm), p->startline);
    } else {
        u32 lo = simm & 0xFFF;
        if (lo >= 0x800) lo -= 0x1000;
        u32 hi = (u32)(simm - lo) >> 12;
        asm_emit(LUI(d, hi), p->startline);
        asm_emit(ADDI(d, d, lo), p->startline);
    }
    return NULL;
}

const char *handle_la(Parser *p, const char *opcode, size_t opcode_len) {
    Parser orig = *p;
    int d;
    bool later;

    skip_whitespace(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";
    skip_whitespace(p);
    if (!consume_if(p, ',')) return "Expected ,";

    u32 addr;
    skip_whitespace(p);
    const char *err = label(p, &orig, handle_la, opcode, opcode_len, &addr,
                            &later, reloc_hi20lo12i);
    if (later) {
        asm_emit(0, p->startline);
        asm_emit(0, p->startline);
        return NULL;
    }
    if (err) return err;
    i32 simm = addr - (g_section->emit_idx + g_section->base);

    u32 lo = simm & 0xFFF;
    if (lo >= 0x800) lo -= 0x1000;
    u32 hi = (u32)(simm - lo) >> 12;
    asm_emit(AUIPC(d, hi), p->startline);
    asm_emit(ADDI(d, d, lo), p->startline);
    return NULL;
}

const char *handle_ecall(Parser *p, const char *opcode, size_t opcode_len) {
    asm_emit(0x73, p->startline);
    return NULL;
}

const char *handle_sret(Parser *p, const char *opcode, size_t opcode_len) {
    asm_emit(0x10200073, p->startline);
    return NULL;
}

const char *handle_csr(Parser *p, const char *opcode, size_t opcode_len) {
    int csr, d, s;

    skip_whitespace(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";

    skip_whitespace(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_whitespace(p);
    if ((csr = parse_csr(p)) == -1) return "Invalid CSR";

    skip_whitespace(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_whitespace(p);
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

    skip_whitespace(p);
    if ((d = parse_reg(p)) == -1) return "Invalid rd";

    skip_whitespace(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_whitespace(p);
    if ((csr = parse_csr(p)) == -1) return "Invalid CSR";

    skip_whitespace(p);
    if (!consume_if(p, ',')) return "Expected ,";

    skip_whitespace(p);
    if (!parse_numeric(p, &zimm)) return "Invalid imm";

    u32 inst = 0;
    if (str_eq_case(opcode, opcode_len, "csrrwi")) inst = CSRRWI(d, zimm, csr);
    else if (str_eq_case(opcode, opcode_len, "csrrsi"))
        inst = CSRRSI(d, zimm, csr);
    else if (str_eq_case(opcode, opcode_len, "csrrci"))
        inst = CSRRCI(d, zimm, csr);

    asm_emit(inst, p->startline);
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
         "mul", "mulh", "mulu", "mulhu", "div", "divu", "rem", "remu"},
    },
    {handle_alu_imm,
     {"addi", "slt", "sltiu", "andi", "ori", "xori", "slli", "srli", "srai"}},
    {handle_ldst, {"lb", "lh", "lw", "lbu", "lhu", "sb", "sh", "sw"}},
    {handle_branch,
     {"beq", "bne", "blt", "bge", "bltu", "bgeu", "bgt", "ble", "bgtu",
      "bleu"}},
    {handle_branch_zero, {"beqz", "bnez", "blez", "bgez", "bltz", "bgtz"}},
    {handle_alu_pseudo, {"mv", "not", "neg", "seqz", "snez", "sltz", "sgtz"}},
    {handle_jump, {"j", "jal"}},
    {handle_jump_reg, {"jr", "jalr"}},
    {handle_ret, {"ret"}},
    {handle_upper, {"lui", "auipc"}},
    {handle_li, {"li"}},
    {handle_la, {"la"}},
    {handle_ecall, {"ecall"}},
    {handle_csr, {"csrrw", "csrrs", "csrrc"}},
    {handle_csr_imm, {"csrrwi", "csrrsi", "csrrci"}},
    {handle_sret, {"sret"}},
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
#define MMIO_LABEL(name, addrr)                                      \
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

export void assemble(const char *txt, size_t s, bool allow_externs) {
    g_allow_externs = allow_externs;
    g_in_fixup = false;

    callsan_init();
    emulator_init();

    g_text = malloc(sizeof(*g_text));
    ARES_CHECK_OOM(g_text);
    g_data = malloc(sizeof(*g_data));
    ARES_CHECK_OOM(g_data);
    g_kernel_data = malloc(sizeof(*g_kernel_data));
    ARES_CHECK_OOM(g_kernel_data);
    g_kernel_text = malloc(sizeof(*g_kernel_text));
    ARES_CHECK_OOM(g_kernel_text);

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
            parse_ident(p, &directive, &directive_len);
            skip_whitespace(p);

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
                skip_whitespace(p);
                const char *ident;
                size_t ident_len;
                parse_ident(p, &ident, &ident_len);
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
                i32 value;
                bool first = true;
                while (true) {
                    skip_whitespace(p);
                    if (first || consume_if(p, ',')) {
                        skip_whitespace(p);
                        if (!parse_numeric(p, &value)) {
                            err = "Invalid word";
                            break;
                        }
                        asm_emit(value, p->startline);
                    } else break;
                    first = false;
                }
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

        if (consume_if(p, ':')) {
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

        // see comment above skip_trailing on why this is distinct from
        // skip_whitespace
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
            (!closest ||
             ARES_ARRAY_GET(&g_labels, i)->addr > closest->addr)) {
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

// Ugly because i"m calling it from JS"
// The problem with this is that... it's basically the cleanest way to do it
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

void prepare_aux_sections() {
    g_stack = malloc(sizeof(Section));
    ARES_CHECK_OOM(g_stack);
    *g_stack = (Section){.name = "ARES_STACK",
                         .base = STACK_TOP - STACK_LEN,
                         .limit = STACK_TOP,
                         .contents = ARES_ARRAY_PREPARE(u8, STACK_LEN),
                         .emit_idx = 0,
                         .align = 1,
                         .relocations = {.buf = NULL, .len = 0, .cap = 0},
                         .read = true,
                         .write = true,
                         .execute = false,
                         .physical = false};

    g_stack->contents.buf = malloc(g_stack->contents.len);
    // fill all the memory with random uninitialized values
    memset(g_stack->contents.buf, 0xAB, g_stack->contents.len);

    g_regs[2] = STACK_TOP;  // FIXME: now i am diverging from RARS, which
                            // does STACK_TOP - 4

    g_mmio = malloc(sizeof(*g_mmio));
    ARES_CHECK_OOM(g_mmio);
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

void prepare_runtime_sections() {
    // TODO: dynamically growing stacks?

    *ARES_ARRAY_PUSH(&g_sections) = g_text;
    *ARES_ARRAY_PUSH(&g_sections) = g_data;
    *ARES_ARRAY_PUSH(&g_sections) = g_kernel_text;
    *ARES_ARRAY_PUSH(&g_sections) = g_kernel_data;
}

void free_runtime() {
    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_sections); i++) {
        Section *s = *ARES_ARRAY_GET(&g_sections, i);
        ARES_ARRAY_FREE(&s->relocations);
        ARES_ARRAY_FREE(&s->contents);
        free(s);
    }

    ARES_ARRAY_FREE(&g_sections);
    ARES_ARRAY_FREE(&g_text_by_linenum);
    ARES_ARRAY_FREE(&g_labels);
    ARES_ARRAY_FREE(&g_deferred_insn);
    ARES_ARRAY_FREE(&g_globals);
    ARES_ARRAY_FREE(&g_externs);
    ARES_ARRAY_FREE(&g_shadow_stack);
}

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "../exec/ares/core.h"
#include "../exec/ares/emulate.h"

void setUp(void) {}
void tearDown(void) { free_runtime(); }

// need this wrapper because TEST_ASSERT_EQUAL_STRING_LEN doesn't check that the
// length matches
#define TEST_ASSERT_EQUAL_STR(x, y, z)         \
    {                                          \
        TEST_ASSERT_EQUAL(strlen(x), z);       \
        TEST_ASSERT_EQUAL_STRING_LEN(x, y, z); \
    }

static Parser init_parser(const char *str) {
    Parser p;
    p.input = str;
    p.pos = 0;
    p.size = strlen(str);
    p.lineidx = 1;
    return p;
}

void test_parse_numeric_decimal(void) {
    Parser p = init_parser("+-+-123");
    int result = 0;
    bool ok = parse_numeric(&p, &result);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(123, result);
}

void test_parse_numeric_invalid(void) {
    Parser p = init_parser("0x");
    int result = 0;
    bool ok = parse_numeric(&p, &result);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_UINT(0, p.pos);
}

void test_invalid_literals(void) {
    Parser p = init_parser("0b102");
    int result = 0;
    bool ok = parse_numeric(&p, &result);
    TEST_ASSERT_FALSE(ok);
}

void test_parse_numeric_unterminated_char(void) {
    Parser p = init_parser("'a");
    int result = 0;
    bool ok = parse_numeric(&p, &result);
    TEST_ASSERT_FALSE(ok);
}

void test_parse_quoted_str_valid(void) {
    char *out = NULL;
    size_t out_len = 0;
    Parser p = init_parser("\"hello\\n\"");
    bool ok = parse_quoted_str(&p, &out, &out_len);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STR("hello\n", out, out_len);
    free(out);
}

void test_parse_quoted_str_unterminated(void) {
    char *out = NULL;
    size_t out_len = 0;
    Parser p = init_parser("\"unterminated");
    bool ok = parse_quoted_str(&p, &out, &out_len);
    TEST_ASSERT_FALSE(ok);
}

void test_parse_quoted_harder(void) {
    char *out = NULL;
    size_t out_len = 0;
    Parser p = init_parser("\"printf(\\\"Hello\\\")\"");
    bool ok = parse_quoted_str(&p, &out, &out_len);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STR("printf(\"Hello\")", out, out_len);
    free(out);
}

void test_parse_quoted_backslash(void) {
    Parser p = init_parser("\"C:\\\\Users\\\\\"");
    char *out = NULL;
    size_t out_len = 0;
    bool ok = parse_quoted_str(&p, &out, &out_len);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STR("C:\\Users\\", out, out_len);
    free(out);
}

void test_skip_comment_line_invaid(void) {
    Parser p = init_parser("/a this is invalid");
    bool skipped = skip_comment(&p);
    TEST_ASSERT_FALSE(skipped);
}

void test_skip_comment_multiline2(void) {
    Parser p = init_parser("/* nonterminated *");
    bool skipped = skip_comment(&p);
    TEST_ASSERT_TRUE(skipped);
    TEST_ASSERT_EQUAL(p.pos, p.size);
}

void test_skip_comment_block(void) {
    Parser p = init_parser("/* block comment */123");
    bool skipped = skip_comment(&p);
    TEST_ASSERT_TRUE(skipped);
    TEST_ASSERT_EQUAL('1', p.input[p.pos]);
}

void test_skip_whitespace(void) {
    Parser p = init_parser("   \n//comment\n  \t789");
    skip_whitespace(&p);
    TEST_ASSERT_TRUE(p.pos < p.size);
    TEST_ASSERT_EQUAL('7', p.input[p.pos]);
}

void test_invalid_literal(void) {
    Parser p = init_parser("-abc");
    int result = 0;
    bool ok = parse_numeric(&p, &result);
    TEST_ASSERT_FALSE(ok);
}

void test_parse_quoted_str_invalid_escape(void) {
    Parser p = init_parser("\"hello\\x\"");
    char *out = NULL;
    size_t out_len = 0;
    bool ok = parse_quoted_str(&p, &out, &out_len);
    TEST_ASSERT_FALSE(ok);
}

void assemble_line(const char *line) { assemble(line, strlen(line), false); }

void test_unknown_opcode(void) {
    assemble_line("unhandled");
    TEST_ASSERT_EQUAL_STRING(g_error, "Unknown opcode");
}

void test_unterminated_instruction(void) {
    assemble_line("addi x1, x2 ");
    TEST_ASSERT_EQUAL_STRING(g_error, "Expected ,");
}

void test_addi_immediate_out_of_range(void) {
    assemble_line("addi x1, x2, 3000");
    TEST_ASSERT_EQUAL_STRING(g_error, "Out of bounds immediate");
}

void test_lui_immediate_out_of_range(void) {
    assemble_line("lui x1, 1048576");
    TEST_ASSERT_EQUAL_STRING(g_error, "Out of bounds immediate");
}

void test_number_parsing_i64_overflow(void) {
    assemble_line(".data\n.word 1234567890123456789012345678901234567890");
    TEST_ASSERT_EQUAL_STRING(g_error, "Invalid word");
}

void test_number_parsing_u32_overflow(void) {
    assemble_line(".data\n.word 4294967296");
    TEST_ASSERT_EQUAL_STRING(g_error, "Invalid word");
}

void test_number_parsing_i32_overflow(void) {
    assemble_line(".data\n.word -2147483649");
    TEST_ASSERT_EQUAL_STRING(g_error, "Invalid word");
}

void test_number_parsing_u32_edge(void) {
    assemble_line(".data\n.word -2147483648");
    TEST_ASSERT_NULL(g_error);
}

void test_sw_immediate_out_of_range(void) {
    assemble_line("sw x1, 5000(x2)");
    TEST_ASSERT_EQUAL_STRING(g_error, "Out of bounds immediate");
}

void test_sw_immediate_negative(void) {
    bool err;
    assemble_line("sw x1, -1(x2)");
    TEST_ASSERT_EQUAL_INT(g_text->contents.len, 4);
    u32 word = LOAD(g_text->base, 4, &err);
    TEST_ASSERT_EQUAL_INT(word, 0xfe112fa3);
}

void test_instruction_trailing_comma(void) {
    assemble_line("addi x1, x2, 300,");
    TEST_ASSERT_EQUAL_STRING(g_error, "Expected newline");
}

void test_addi_oob(void) {
    assemble_line("addi x1, x2, 2048");
    TEST_ASSERT_EQUAL_STRING(g_error, "Out of bounds immediate");
    free_runtime();

    assemble_line("addi x1, x2, -2049");
    TEST_ASSERT_EQUAL_STRING(g_error, "Out of bounds immediate");
}

void test_lui_oob(void) {
    assemble_line("lui x1, 0x100000");
    TEST_ASSERT_EQUAL_STRING(g_error, "Out of bounds immediate");
}

void test_add_invalid_reg(void) {
    assemble_line("add x1, x2, x32");
    TEST_ASSERT_EQUAL_STRING(g_error, "Invalid rs2");
}

void test_add_invalid_reg_2(void) {
    assemble_line("add xb, xc, xa");
    TEST_ASSERT_EQUAL_STRING(g_error, "Invalid rd");
}

void test_add_invalid_reg_3(void) {
    assemble_line("add x0, x0, x08");
    TEST_ASSERT_EQUAL_STRING(g_error, "Invalid rs2");
}

void test_shift_invalid_imm_1(void) {
    assemble_line("srai x0, x0, 32");
    TEST_ASSERT_EQUAL_STRING(g_error, "Invalid shift immediate");
}

void test_shift_invalid_imm_2(void) {
    assemble_line("srai x0, x0, -6");
    TEST_ASSERT_EQUAL_STRING(g_error, "Invalid shift immediate");
}

void test_add_invalid_reg_4(void) {
    assemble_line("add x0, x$, x0");
    TEST_ASSERT_EQUAL_STRING(g_error, "Invalid rs1");
}

void test_case_insensitivity(void) {
    assemble_line("ADDI X1, X2, 0X41");
    TEST_ASSERT_EQUAL_STRING(g_error, NULL);
}

void test_instruction_with_trailing_garbage(void) {
    assemble_line("addi x1, x2, 1000 garbage");
    TEST_ASSERT_EQUAL_STRING(g_error, "Expected newline");
}

void test_parse_directives_nums(void) {
    bool err;
    assemble_line(".data\nvar: .WORD 5");
    TEST_ASSERT_EQUAL_INT(g_data->contents.len, 4);
    u32 word = LOAD(g_data->base, 4, &err);
    TEST_ASSERT_EQUAL_INT(word, 5);
    free_runtime();

    assemble_line(".DATA\nvar: .HALF 5");
    TEST_ASSERT_EQUAL_INT(g_data->contents.len, 2);
    u16 half = LOAD(g_data->base, 2, &err);
    TEST_ASSERT_EQUAL_INT(half, 5);
    free_runtime();

    assemble_line(".data\nvar: .byte 5");
    TEST_ASSERT_EQUAL_INT(g_data->contents.len, 1);
    u8 byte = LOAD(g_data->base, 1, &err);
    TEST_ASSERT_EQUAL_INT(byte, 5);
}

void test_parse_directives_nums_invalid(void) {
    assemble_line(".data\nvar: .word 0xG");
    TEST_ASSERT_EQUAL_STRING(g_error, "Invalid word");
}

void test_parse_directives_nums_oob(void) {
    assemble_line(".data\nvar: .half 0x10000");
    TEST_ASSERT_EQUAL_STRING(g_error, "Out of bounds half");
    free_runtime();

    assemble_line(".data\nvar: .half -32769");
    TEST_ASSERT_EQUAL_STRING(g_error, "Out of bounds half");
    free_runtime();

    assemble_line(".data\nvar: .byte 0x100");
    TEST_ASSERT_EQUAL_STRING(g_error, "Out of bounds byte");
    free_runtime();

    assemble_line(".data\nvar: .byte -129");
    TEST_ASSERT_EQUAL_STRING(g_error, "Out of bounds byte");
    free_runtime();
}

void test_parse_numeric_hex_prefix_no_digits(void) {
    assemble_line(".data\nvar: .word 0x");
    TEST_ASSERT_EQUAL_STRING(g_error, "Invalid word");
}
void test_parse_numeric_bin_prefix_no_digits(void) {
    assemble_line(".data\nvar: .word 0b");
    TEST_ASSERT_EQUAL_STRING(g_error, "Invalid word");
}

void test_branch_cross_section_label(void) {
    assemble_line(".data\nfoo: .word 0\n.text\nbeq a0, a1, foo");
    TEST_ASSERT_NOT_NULL(g_error);
}

void test_jalr_sign_before_paren(void) {
    assemble_line("jalr a0, -(a1)");
    TEST_ASSERT_NOT_NULL(g_error);
}

void test_jalr_1(void) {
    bool err = false;
    assemble_line("jalr x4");
    TEST_ASSERT_EQUAL(0x000200e7, LOAD(g_text->base, 4, &err));
}

void test_jalr_2(void) {
    bool err = false;
    assemble_line("jalr x4, x5");
    TEST_ASSERT_EQUAL(0x00028267, LOAD(g_text->base, 4, &err));
}

void test_jalr_3(void) {
    bool err = false;
    assemble_line("jalr x4, x5, 6");
    TEST_ASSERT_EQUAL(0x00628267, LOAD(g_text->base, 4, &err));
}

void test_jalr_4(void) {
    bool err = false;
    assemble_line("jalr x4, 6(x5)");
    TEST_ASSERT_EQUAL(0x00628267, LOAD(g_text->base, 4, &err));
}

void test_data_label_1(void) {
    bool err = false;
    assemble_line(".data\nhi: .word label\n.text\nlabel:\nnop");
    TEST_ASSERT_EQUAL(TEXT_BASE, LOAD(g_data->base, 4, &err));
}

void test_data_label_2(void) {
    bool err = false;
    assemble_line(".text\nlabel:\nnop\n.data\nhi: .word label\n");
    TEST_ASSERT_EQUAL(TEXT_BASE, LOAD(g_data->base, 4, &err));
}

void test_data_label_3(void) {
    bool err = false;
    assemble_line(".text\nlabel:\nnop\n.data\nhi: .word labelNotFound\n");
    TEST_ASSERT_EQUAL_STRING(g_error, "Invalid word");
}

void test_data_label_4(void) {
    bool err = false;
    assemble_line(".data\nhi: .word label, 2\n.text\nlabel:\nnop");
    TEST_ASSERT_EQUAL(TEXT_BASE, LOAD(g_data->base, 4, &err));
    TEST_ASSERT_EQUAL(2, LOAD(g_data->base + 4, 4, &err));
}

void test_parse_string_bad_escape(void) {
    assemble_line(".data\n.asciz \"hello\\q\"");
    TEST_ASSERT_EQUAL_STRING(g_error, "Invalid string");
}

void test_branch_forward_out_of_range(void) {
    char buf[8192 + 64];
    strcpy(buf, ".text\nbeq a0, a1, far\n");
    for (int i = 0; i < 2048; i++) strcat(buf, "nop\n");
    strcat(buf, "far:\nnop");
    assemble_line(buf);
    TEST_ASSERT_EQUAL_STRING(g_error, "Branch immediate too large");
}

void test_globl_without_definition(void) {
    assemble_line(".text\n.globl foo\nret");
    TEST_ASSERT_NULL(g_error);
    u32 addr;
    Section *sec;
    TEST_ASSERT_FALSE(resolve_symbol("foo", 3, true, &addr, &sec));
}

void test_parse_directives_str(void) {
    assemble_line(".data\nstr: .ASCII \"hi\", \"hi\"");
    TEST_ASSERT_EQUAL_STR("hihi", g_data->contents.buf, g_data->contents.len);
    free_runtime();

    assemble_line(".data\nstr: .string \"hi\"");
    TEST_ASSERT_EQUAL_INT(g_data->contents.len, 3);
    TEST_ASSERT_EQUAL_CHAR_ARRAY("hi\0", g_data->contents.buf,
                                 g_data->contents.len);
    free_runtime();
}

void test_unconsumed_str(void) {
    assemble_line(".data\nstr: .ascii");
    TEST_ASSERT_EQUAL_STRING(g_error, "Invalid string");
}

void test_unterminated_str(void) {
    assemble_line(".data\nstr: .ascii \"hi");
    TEST_ASSERT_EQUAL_STRING(g_error, "Invalid string");
}

#define STRING_SIZE(s) (sizeof(s) - 1)

void test_parse_multiple_definitions(void) {
    assemble_line(".data\nvar: .word 5\nvar: .word 10");
    TEST_ASSERT_EQUAL_STRING(g_error,
                             "Multiple definitions for the same label");
}

void test_parse_symbol_resolution_error(void) {
    assemble_line("j unknown_symbol\n");
    TEST_ASSERT_EQUAL_STRING("Label not found", g_error);
}

void test_resolve_text_symbol_found(void) {
    assemble_line("mylabel: addi a0, a0, 0");
    u32 addr;
    Section *sec;
    bool found =
        resolve_symbol("mylabel", strlen("mylabel"), false, &addr, &sec);
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL(sec, g_text);
    TEST_ASSERT_TRUE(addr >= g_text->base && addr < g_text->limit);
}

void test_resolve_data_symbol_found(void) {
    assemble_line(".data\nmylabel: .word 1234");
    u32 addr;
    Section *sec;
    bool found =
        resolve_symbol("mylabel", strlen("mylabel"), false, &addr, &sec);
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL(sec, g_data);
    TEST_ASSERT_TRUE(addr >= g_data->base && addr < g_data->limit);
}

void test_pc_to_label_r2(void) {
    assemble_line("label: add x0, x0, x0");
    LabelData *ret = NULL;
    u32 off = 0;
    bool result = pc_to_label_r(g_text->base, &ret, &off);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_STR("label", ret->txt, ret->len);
}

void test_pc_to_label_r_no_label(void) {
    assemble_line("add x0, x0, x0");
    LabelData *ret = NULL;
    u32 off = 0;
    bool result = pc_to_label_r(0xdeadbeef, &ret, &off);
    TEST_ASSERT_FALSE(result);
}

void test_fixup(void) {
    assemble_line("j exit\nexit:");
    bool err;
    TEST_ASSERT_EQUAL_INT(LOAD(g_text->base, 4, &err), 0x0040006f);
    TEST_ASSERT_FALSE(err);
}

void test_backtrack(void) {
    assemble_line("j .exit\n.exit:");
    TEST_ASSERT_EQUAL_STRING(g_error, NULL);
    bool err;
    TEST_ASSERT_EQUAL_INT(LOAD(g_text->base, 4, &err), 0x0040006f);
    TEST_ASSERT_FALSE(err);
}

void test_dotlabel_fail(void) {
    assemble_line("j .data\n.data:");
    TEST_ASSERT_NOT_NULL(g_error);
}


void test_globl_empty(void) {
    assemble_line(".globl\n");
    TEST_ASSERT_EQUAL_STRING(g_error, "Expected identifier after .globl");
}

void test_nolabel(void) {
    assemble_line("j ");
    TEST_ASSERT_EQUAL_STRING(g_error, "No label");
}

void test_nonglobal_start(void) {
    assemble_line("_start: ");
    TEST_ASSERT_EQUAL_STRING(g_error, "_start defined, but without .globl");
}

void test_start_in_data(void) {
    assemble_line(".globl _start\n.data\n_start: ");
    TEST_ASSERT_EQUAL_STRING(g_error, "_start not in .text section");
}

void test_parse_numeric_char_escape_newline(void) {
    Parser p = init_parser("'\\n'");
    i32 result = 0;
    TEST_ASSERT_TRUE(parse_numeric(&p, &result));
    TEST_ASSERT_EQUAL_INT('\n', result);
}

void test_parse_numeric_char_escape_null(void) {
    Parser p = init_parser("'\\0'");
    i32 result = 0;
    TEST_ASSERT_TRUE(parse_numeric(&p, &result));
    TEST_ASSERT_EQUAL_INT(0, result);
}

void test_parse_numeric_char_literal(void) {
    Parser p = init_parser("'A'");
    i32 result = 0;
    TEST_ASSERT_TRUE(parse_numeric(&p, &result));
    TEST_ASSERT_EQUAL_INT('A', result);
}

void test_parse_numeric_char_bad_escape(void) {
    Parser p = init_parser("'\\q'");
    i32 result = 0;
    TEST_ASSERT_FALSE(parse_numeric(&p, &result));
}

void test_skip_comment_line(void) {
    const char *str = "// line comment\n123";
    Parser p = init_parser(str);
    TEST_ASSERT_TRUE(skip_comment(&p));
    TEST_ASSERT_EQUAL_CHAR('\n', str[p.pos]);
}

void test_skip_comment_hash(void) {
    const char *str = "# preprocessor\n456";
    Parser p = init_parser("# preprocessor\n456");
    TEST_ASSERT_TRUE(skip_comment(&p));
    TEST_ASSERT_EQUAL_CHAR('\n', str[p.pos]);
}

void test_skip_whitespace_updates_lineidx(void) {
    Parser p = init_parser("\n\n\nret");
    skip_whitespace(&p);
    TEST_ASSERT_EQUAL_INT(4, p.lineidx);
}

void test_word_list(void) {
    assemble_line(".data\n.word 1, 2, 3");
    TEST_ASSERT_NULL(g_error);
    TEST_ASSERT_EQUAL_INT(12, g_data->contents.len);
    bool err;
    TEST_ASSERT_EQUAL_INT(1, LOAD(g_data->base, 4, &err));
    TEST_ASSERT_EQUAL_INT(2, LOAD(g_data->base + 4, 4, &err));
    TEST_ASSERT_EQUAL_INT(3, LOAD(g_data->base + 8, 4, &err));
}

void test_ascii_no_null_terminator(void) {
    assemble_line(".data\n.ascii \"hi\"");
    TEST_ASSERT_NULL(g_error);
    TEST_ASSERT_EQUAL_INT(2, g_data->contents.len);
}

void test_asciz_null_terminator(void) {
    assemble_line(".data\n.asciz \"hi\"");
    TEST_ASSERT_NULL(g_error);
    TEST_ASSERT_EQUAL_INT(3, g_data->contents.len);
    TEST_ASSERT_EQUAL_CHAR('\0', g_data->contents.buf[2]);
}

void test_label_address_increments(void) {
    assemble_line(".data\nfoo: .word 1\nbar: .word 2");
    TEST_ASSERT_NULL(g_error);
    u32 foo_addr, bar_addr;
    Section *sec;
    resolve_symbol("foo", 3, false, &foo_addr, &sec);
    resolve_symbol("bar", 3, false, &bar_addr, &sec);
    TEST_ASSERT_EQUAL_INT(4, bar_addr - foo_addr);
}

void test_section_switch(void) {
    assemble_line(
        ".data\nfoo: .word 99\n.text\nadd x0,x0,x0\n.data\nbar: .word 88");
    TEST_ASSERT_NULL(g_error);
    TEST_ASSERT_EQUAL_INT(8, g_data->contents.len);
    TEST_ASSERT_EQUAL_INT(4, g_text->contents.len);
}

void test_resolve_symbol_not_found(void) {
    assemble_line("ret");
    u32 addr;
    Section *sec;
    TEST_ASSERT_FALSE(resolve_symbol("foo", 5, false, &addr, &sec));
}

void test_multiline_block_comment_in_code(void) {
    assemble_line("/* comment */\nret\n/* another */");
    TEST_ASSERT_NULL(g_error);
    TEST_ASSERT_EQUAL_INT(4, g_text->contents.len);
}

void test_label_at_end_of_file(void) {
    assemble_line(".text\nfoo:");
    TEST_ASSERT_NULL(g_error);
    u32 addr;
    Section *sec;
    TEST_ASSERT_TRUE(resolve_symbol("foo", 3, false, &addr, &sec));
}

void test_unknown_section(void) {
    assemble_line(".section .foo");
    TEST_ASSERT_EQUAL_STRING(g_error, "Section not found");
}

void test_la_deferred(void) {
    bool err;
    assemble_line("la t0, hi\nhi: nop");
    TEST_ASSERT_EQUAL_INT(LOAD(g_text->base, 4, &err), 0x00000297);
    TEST_ASSERT_EQUAL_INT(LOAD(g_text->base + 4, 4, &err), 0x00828293);
}

void test_linenum(void) {
    assemble_line(
        "\
addi x0, x0, 1 \n\
addi x0, x0, 2 \n\
               \n\
addi x0, x0, 3 \n\
");
    TEST_ASSERT_EQUAL_INT(g_text->by_linenum.buf[0], 1);
    TEST_ASSERT_EQUAL_INT(g_text->by_linenum.buf[1 * 4], 2);
    TEST_ASSERT_EQUAL_INT(g_text->by_linenum.buf[2 * 4], 4);
    TEST_ASSERT_EQUAL_INT(g_text->by_linenum.len, 3 * 4);
}

void test_linenum_2(void) {
    assemble_line(
        "\
.globl _start\n\
.data\n\
    num1: .word 5\n\
    num2: .word 6\n\
.text\n\
_start:\n\
    jal foo\n\
foo:\n\
    la    a0, num1\n\
    la    a1, num2\n\
    lw    a0, 0(a0)\n\
    lw    a1, 0(a1)\n\
");
    TEST_ASSERT_EQUAL_INT(g_text->by_linenum.len, 7 * 4);
}

void test_hi_lo_int(void) {
    bool err;
    u32 expected_lui, expected_addi;
    assemble_line("li a0, 123456\n");
    expected_lui = LOAD(g_text->base, 4, &err);
    expected_addi = LOAD(g_text->base + 4, 4, &err);
    free_runtime();
    g_error = NULL;
    assemble_line("lui a0, %hi(123456)\naddi a0, a0, %lo(123456)\n");
    TEST_ASSERT_EQUAL_INT(expected_lui, LOAD(g_text->base, 4, &err));
    TEST_ASSERT_EQUAL_INT(expected_addi, LOAD(g_text->base + 4, 4, &err));
}

void test_hi_lo_label(void) {
    bool err;
    u32 expected_lui, expected_addi;
    assemble_line("li a0, 0x00400008\n");
    expected_lui = LOAD(g_text->base, 4, &err);
    expected_addi = LOAD(g_text->base + 4, 4, &err);
    free_runtime();
    assemble_line("lui a0, %hi(label)\naddi a0, a0, %lo(label)\nlabel:");
    TEST_ASSERT_EQUAL_INT(expected_lui, LOAD(g_text->base, 4, &err));
    TEST_ASSERT_EQUAL_INT(expected_addi, LOAD(g_text->base + 4, 4, &err));
}

void test_la_vs_pcrel(void) {
    bool err;
    u32 expected_auipc, expected_addi;
    assemble_line(
        "l0: auipc a0, %pcrel_hi(label)\naddi a0, a0, %pcrel_lo(l0)\nlabel:");
    TEST_ASSERT_EQUAL_INT(0x517, LOAD(g_text->base, 4, &err));
    TEST_ASSERT_EQUAL_INT(0x00850513, LOAD(g_text->base + 4, 4, &err));
}

// -- runtime tests

void build_and_run(const char *txt) {
    u32 addr;
    assemble(txt, strlen(txt), false);
    TEST_ASSERT_EQUAL_STRING(g_error, NULL);
    if (resolve_symbol("_start", strlen("_start"), true, &addr, NULL))
        g_pc = addr;
    while (!g_exited) {
        emulate();
        if (g_runtime_error_type != ERROR_NONE) break;
    }
}
void check_pc_at_label(const char *label) {
    u32 addr;
    TEST_ASSERT_TRUE(resolve_symbol(label, strlen(label), false, &addr, NULL));
    TEST_ASSERT_EQUAL(g_pc, addr);
}
void test_runtime_exit(void) {
    build_and_run("li a7, 93\necall");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_NONE);
}

// sd x0, 0(x0): 64 bit instruction, currently unhandled
void test_runtime_unhandled(void) {
    build_and_run(
        "\
.globl _start   \n\
_start:         \n\
E:  .word 0x00003023  \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_UNHANDLED_INSN);
    check_pc_at_label("E");
}

void test_callsan_clobbered(void) {
    build_and_run(
        "\
fn:                \n\
    ret            \n\
.globl _start      \n\
_start:            \n\
    li a3, 2       \n\
    jal fn         \n\
E:  addi a3, a3, 1 \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_CALLSAN_CALL_CLOBBERED);
    TEST_ASSERT_EQUAL(g_runtime_error_params[0], REG_A3);
    check_pc_at_label("E");
}

void test_callsan_cantread_1(void) {
    build_and_run(
        "\
fn:                \n\
    ret            \n\
.globl _start      \n\
_start:            \n\
E:  addi a3, a3, 1 \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_CALLSAN_CANTREAD);
    TEST_ASSERT_EQUAL(g_runtime_error_params[0], REG_A3);
    check_pc_at_label("E");
}

void test_callsan_cantread_2(void) {
    build_and_run(
        "\
fn:                \n\
    ret            \n\
.globl _start      \n\
_start:            \n\
    jal fn         \n\
E:  addi a3, a3, 1 \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_CALLSAN_CANTREAD);
    TEST_ASSERT_EQUAL(g_runtime_error_params[0], REG_A3);
    check_pc_at_label("E");
}

void test_callsan_not_saved(void) {
    build_and_run(
        "\
fn:             \n\
    li s1, 1234 \n\
E:  ret         \n\
.globl _start   \n\
_start:         \n\
    jal fn      \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_CALLSAN_NOT_SAVED);
    TEST_ASSERT_EQUAL(g_runtime_error_params[0], REG_S1);
    check_pc_at_label("E");
}

void test_callsan_ra_mismatch(void) {
    build_and_run(
        "\
fn2:                 \n\
    ret              \n\
fn:                  \n\
    jal fn2          \n\
E:  ret              \n\
.globl _start        \n\
_start:              \n\
    jal fn           \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_CALLSAN_RA_MISMATCH);
    check_pc_at_label("E");
}

void test_callsan_sp_mismatch(void) {
    build_and_run(
        "\
fn:                  \n\
    addi sp, sp, -16 \n\
    addi sp, sp, 24  \n\
E:  ret              \n\
.globl _start        \n\
_start:              \n\
    jal fn           \n\
    li a7, 93        \n\
    ecall            \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_CALLSAN_SP_MISMATCH);
    check_pc_at_label("E");
}

void test_callsan_ret_empty(void) {
    build_and_run(
        "\
fn:                  \n\
    addi sp, sp, -16 \n\
    addi sp, sp, 16  \n\
    ret              \n\
.globl _start        \n\
_start:              \n\
    jal fn           \n\
E:  ret              \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_CALLSAN_RET_EMPTY);
    check_pc_at_label("E");
}

// first set of accesses should be valid, second no
void test_callsan_load_stack(void) {
    build_and_run(
        "\
fn:                 \n\
    addi sp, sp, -8 \n\
    sw ra, 0(sp)    \n\
    lw ra, 0(sp)    \n\
E:  lw ra, 4(sp)    \n\
    sw ra, 4(sp)    \n\
    addi sp, sp, 8  \n\
    ret             \n\
.globl _start       \n\
_start:             \n\
    jal fn          \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_CALLSAN_LOAD_STACK);
    check_pc_at_label("E");
}

void test_callsan_stack_poison_fresh(void) {
    build_and_run(
        "\
fn:                 \n\
    addi sp, sp, -4 \n\
E:  lw t0, 0(sp)    \n\
    addi sp, sp, 4  \n\
    ret             \n\
.globl _start       \n\
_start:             \n\
    jal fn          \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_CALLSAN_LOAD_STACK);
    check_pc_at_label("E");
}

void test_callsan_stack_poison_after_ret(void) {
    build_and_run(
        "\
fn:                 \n\
    addi sp, sp, -4 \n\
    sw ra, 0(sp)    \n\
    lw ra, 0(sp)    \n\
    addi sp, sp, 4  \n\
    ret             \n\
.globl _start       \n\
_start:             \n\
    jal fn          \n\
E:  lw t1, -4(sp)   \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_CALLSAN_LOAD_STACK);
    check_pc_at_label("E");
}

void test_callsan_stack_poison_second_call(void) {
    build_and_run(
        "\
fn:                 \n\
    addi sp, sp, -4 \n\
    sw ra, 0(sp)    \n\
    lw ra, 0(sp)    \n\
    addi sp, sp, 4  \n\
    ret             \n\
fn_wrong:           \n\
    addi sp, sp, -4 \n\
E:  lw ra, 0(sp)    \n\
    addi sp, sp, 4  \n\
    ret             \n\
.globl _start       \n\
_start:             \n\
    jal fn          \n\
    jal fn_wrong    \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_CALLSAN_LOAD_STACK);
    check_pc_at_label("E");
}

void test_callsan_t_clobbered_inside(void) {
    build_and_run(
        "\
fn:                 \n\
    li t0, 99       \n\
    ret             \n\
.globl _start       \n\
_start:             \n\
    li t0, 1        \n\
    jal fn          \n\
E:  addi t0, t0, 1  \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_CALLSAN_CALL_CLOBBERED);
    TEST_ASSERT_EQUAL(g_runtime_error_params[0], REG_T0);
    check_pc_at_label("E");
}

void test_callsan_caller_after_ret(void) {
    build_and_run(
        "\
fn:                 \n\
    ret             \n\
.globl _start       \n\
_start:             \n\
    addi sp, sp, -4 \n\
    sw s0, 0(sp)    \n\
    li s0, 42       \n\
    jal fn          \n\
    lw s0, 0(sp)    \n\
    addi sp, sp, 4  \n\
    li a7, 93       \n\
    ecall           \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_NONE);
}

void test_callsan_arg_clobbered_outside(void) {
    build_and_run(
        "\
fn:                 \n\
    li a0, 100      \n\
    ret             \n\
.globl _start       \n\
_start:             \n\
    li a2, 50       \n\
    jal fn          \n\
E:  mv t0, a2       \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_CALLSAN_CALL_CLOBBERED);
    TEST_ASSERT_EQUAL(g_runtime_error_params[0], REG_A2);
    check_pc_at_label("E");
}

void test_callsan_two_return_values(void) {
    build_and_run(
        "\
fn:                 \n\
    li a0, 42       \n\
    li a1, 7        \n\
    ret             \n\
.globl _start       \n\
_start:             \n\
    jal fn          \n\
    add t0, a0, a1  \n\
    li a7, 93       \n\
    ecall           \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_NONE);
}

void test_registers_and_arithmetic(void) {
    build_and_run(
        "\
.globl _start\n\
_start:\n\
    addi a0, x0, 5\n\
    addi a1, x0, -3\n\
    li a7, 93\n\
    ecall\n\
");
    TEST_ASSERT_EQUAL_UINT32(5U, g_regs[REG_A0]);
    TEST_ASSERT_EQUAL_UINT32((u32)-3, g_regs[REG_A1]);
}

void test_stack_store_load(void) {
    build_and_run(
        "\
.globl _start\n\
_start:\n\
    addi sp, sp, -16\n\
    li a0, 0x1234\n\
    sw a0, 0(sp)\n\
    lw a1, 0(sp)\n\
    addi sp, sp, 16\n\
    li a7, 93\n\
    ecall\n\
");
    TEST_ASSERT_EQUAL_UINT32(0x1234U, g_regs[REG_A1]);
}

void test_store_load_misc(void) {
    build_and_run(
        "\
.globl _start\n\
_start:\n\
    addi sp, sp, -16\n\
    li a0, 0x123\n\
    la a1, dummy\n\
    sw a0, 0(a1)\n\
    li a0, 0x1234\n\
    sw a0, 0(sp)\n\
    lw a1, 0(sp)\n\
    addi sp, sp, 16\n\
    la a2, dummy\n\
    lw a0, 0(a2)\n\
    li a7, 93\n\
    ecall\n\
.data\n\
    dummy: .word 0\n\
");
    TEST_ASSERT_EQUAL_UINT32(0x1234U, g_regs[REG_A1]);
    TEST_ASSERT_EQUAL_UINT32(0x123U, g_regs[REG_A0]);
}

void test_store_incomplete(void) {
    build_and_run(
        "\
.globl _start\n\
_start:\n\
    li a0, 0x123\n\
    la a1, dummy\n\
    sw a0, 0(a1)\n\
.data\n\
    dummy: .byte 0\n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_STORE);
}

void test_load_store_api(void) {
    assemble_line(".data\nvar: .word 0");
    bool err = false;
    STORE(g_data->base, 0xDEADBEEFu, 4, &err);
    TEST_ASSERT_FALSE(err);
    u32 val = LOAD(g_data->base, 4, &err);
    TEST_ASSERT_FALSE(err);
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, val);
}

void test_kernel_memory_protection(void) {
    const char *prog = ".section .kernel_data\nvar: .word 0xCAFEBABE";
    assemble(prog, strlen(prog), false);
    TEST_ASSERT_EQUAL_STRING(g_error, NULL);

    bool err = false;
    // user mode should not be able to read supervisor memory
    u32 val = LOAD(g_kernel_data->base, 4, &err);
    TEST_ASSERT_TRUE(err);

    // but kernel mode should
    emulator_enter_kernel();
    err = false;
    val = LOAD(g_kernel_data->base, 4, &err);
    TEST_ASSERT_FALSE(err);
    TEST_ASSERT_EQUAL_UINT32(0xCAFEBABEu, val);
}

void step(void) {
    emulate();
    TEST_ASSERT_EQUAL(ERROR_NONE, g_runtime_error_type);
}

void test_ecall_stvec(void) {
    const char *prog =
        "\
.section .kernel_text\n\
handler: addi x0, x0, 0\n\
.section .text\n\
.globl _start\n\
_start: ecall\n\
";
    assemble(prog, strlen(prog), false);
    TEST_ASSERT_EQUAL_STRING(g_error, NULL);
    TEST_ASSERT_TRUE(g_kernel_text->contents.len > 0);
    TEST_ASSERT_TRUE(g_text->contents.len > 0);
    g_pc = g_kernel_text->base;

    g_csr[CSR_STVEC] = g_kernel_text->base;

    u32 addr;
    TEST_ASSERT_TRUE(
        resolve_symbol("_start", strlen("_start"), true, &addr, NULL));
    g_pc = addr;

    emulator_leave_kernel();
    emulate();  // one single instruction
    TEST_ASSERT_EQUAL(g_pc, g_csr[CSR_STVEC]);
    TEST_ASSERT_EQUAL(addr, g_csr[CSR_SEPC]);
}

void test_emulator_interrupt_set_pending(void) {
    const char *prog = "addi x0, x0, 0";
    assemble(prog, strlen(prog), false);
    TEST_ASSERT_EQUAL_STRING(g_error, NULL);

    g_csr[CSR_MIP] = 0;
    g_csr[CSR_STVEC] = 0xAABB00;
    emulator_interrupt_set_pending(CAUSE_SUPERVISOR_TIMER & ~CAUSE_INTERRUPT);
    TEST_ASSERT_TRUE(g_csr[CSR_MIP] &
                     (1u << (CAUSE_SUPERVISOR_TIMER & ~CAUSE_INTERRUPT)));
    emulate();
    TEST_ASSERT_EQUAL_UINT32(g_pc, g_csr[CSR_STVEC]);
    TEST_ASSERT_EQUAL_UINT32(g_text->base, g_csr[CSR_SEPC]);
    TEST_ASSERT_EQUAL_UINT32(CAUSE_SUPERVISOR_TIMER, g_csr[CSR_SCAUSE]);
}

void test_sret_returns_to_sepc(void) {
    assemble_line(
        ".section .kernel_text\n"
        "sret\n"
        "addi x0, x0, 0\n"
        "return_target: addi x0, x0, 0\n");
    TEST_ASSERT_EQUAL_STRING(g_error, NULL);
    u32 addr;
    TEST_ASSERT_TRUE(resolve_symbol("return_target", strlen("return_target"),
                                    false, &addr, NULL));
    g_csr[CSR_SEPC] = addr;
    g_pc = g_kernel_text->base;
    emulator_enter_kernel();
    step();
    TEST_ASSERT_EQUAL(g_pc, g_csr[CSR_SEPC]);
}

void test_jump_to_exception_delegation(void) {
    assemble_line(".section .kernel_text\naddi x0, x0, 1\n.text\necall");
    TEST_ASSERT_EQUAL_STRING(g_error, NULL);

    g_csr[CSR_STVEC] = 0xAABB00u;

    step();

    TEST_ASSERT_EQUAL_UINT32(g_text->base, g_csr[CSR_SEPC]);
    TEST_ASSERT_EQUAL_UINT32(CAUSE_U_ECALL, g_csr[CSR_SCAUSE]);
    TEST_ASSERT_EQUAL_UINT32(g_csr[CSR_STVEC], g_pc);
}

void test_vectored_interrupt_handler(void) {
    const char *prog =
        "\
.section .kernel_text\n\
vector_handlers:\n\
    addi x0, x0, 0\n\
    addi x0, x0, 0\n\
    addi x0, x0, 0\n\
    addi x0, x0, 0\n\
    addi x0, x0, 0\n\
    addi x0, x0, 0\n\
    addi x0, x0, 0\n\
.text\n\
.globl _start\n\
_start: addi x0, x0, 0\n\
";
    assemble(prog, strlen(prog), false);
    TEST_ASSERT_EQUAL_STRING(g_error, NULL);

    u32 vector_handlers;
    TEST_ASSERT_TRUE(resolve_symbol("vector_handlers",
                                    strlen("vector_handlers"), false,
                                    &vector_handlers, NULL));

    g_csr[CSR_STVEC] = vector_handlers | 1;

    emulator_interrupt_set_pending(CAUSE_SUPERVISOR_TIMER & ~CAUSE_INTERRUPT);
    step();

    // delivers interrupt and executes one instruction
    TEST_ASSERT_EQUAL(
        4 + vector_handlers + 4 * (CAUSE_SUPERVISOR_TIMER & ~CAUSE_INTERRUPT),
        g_pc);
}

void test_sstatus_write_mask(void) {
    const char *prog =
        "\
.section .kernel_text\n\
    li t0, -1\n\
    csrrw zero, sstatus, t0\n\
";
    assemble(prog, strlen(prog), false);
    TEST_ASSERT_EQUAL_STRING(g_error, NULL);
    emulator_enter_kernel();
    g_pc = g_kernel_text->base;
    step();
    step();
    TEST_ASSERT(g_csr[CSR_MSTATUS] != -1u);
}

void test_sstatus_bit_manipulation(void) {
    const char *prog =
        "\
.section .kernel_text\n\
    csrrw t0, sstatus, zero\n\
    ori t0, t0, 256\n\
    csrrw zero, sstatus, t0\n\
";
    assemble_line(prog);
    TEST_ASSERT_EQUAL_STRING(g_error, NULL);
    g_pc = g_kernel_text->base;
    emulator_enter_kernel();
    step();
    step();
    step();
    TEST_ASSERT_TRUE(g_csr[CSR_MSTATUS] & STATUS_SPP);
}

void test_sstatus_ecall(void) {
    const char *prog =
        "\
.section .kernel_text\n\
    li t0, 0\n\
    csrrw zero, sstatus, t0\n\
    li t0, 2  # set SIE\n\
    csrrs zero, sstatus, t0\n\
    ecall # should clear SIE and set SPIE\n\
";
    assemble_line(prog);
    TEST_ASSERT_EQUAL_STRING(g_error, NULL);
    g_pc = g_kernel_text->base;
    emulator_enter_kernel();
    step();
    step();
    step();
    step();
    step();
    TEST_ASSERT_FALSE(g_csr[CSR_MSTATUS] & STATUS_SIE);
    TEST_ASSERT_TRUE(g_csr[CSR_MSTATUS] & STATUS_SPIE);
}

void test_rvc_insn_size(void) {
    assemble_line("c.nop\nc.nop\n");
    TEST_ASSERT_EQUAL_INT(g_text->by_linenum.len, 2 + 2);
}

void test_rvc_mixed_width(void) {
    assemble_line("c.nop\nnop\nc.nop\n");
    TEST_ASSERT_EQUAL_INT(g_text->by_linenum.len, 2 + 4 + 2);
}

void test_rvc_runtime_c_addi(void) {
    build_and_run(
        "\
.globl _start    \n\
_start:          \n\
    c.li a0, 10  \n\
    c.addi a0, 5 \n\
    li a7, 93    \n\
    ecall        \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_NONE);
    TEST_ASSERT_EQUAL_UINT32(15U, g_regs[REG_A0]);
}

void test_rvc_runtime_c_mv(void) {
    build_and_run(
        "\
.globl _start    \n\
_start:          \n\
    c.li  a1, 3  \n\
    c.mv  a0, a1 \n\
    li a7, 93    \n\
    ecall        \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_NONE);
    TEST_ASSERT_EQUAL_UINT32(3, g_regs[REG_A0]);
}

void test_rvc_runtime_c_lw_sw(void) {
    build_and_run(
        "\
.globl _start      \n\
_start:            \n\
    la   a0, val   \n\
    c.li a1, 4     \n\
    c.sw a1, 0(a0) \n\
    c.lw a2, 0(a0) \n\
    li a7, 93      \n\
    ecall          \n\
.data              \n\
    val: .word 0   \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_NONE);
    TEST_ASSERT_EQUAL_UINT32(4, g_regs[REG_A2]);
}

void test_rvc_runtime_c_j(void) {
    build_and_run(
        "\
.globl _start  \n\
_start:        \n\
    c.j  done  \n\
    c.li a0, 1 \n\
done:          \n\
    li a7, 93  \n\
    ecall      \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_NONE);
    TEST_ASSERT_EQUAL_UINT32(0U, g_regs[REG_A0]);
}

void test_rvc_runtime_c_beqz(void) {
    build_and_run(
        "\
.globl _start      \n\
_start:            \n\
    c.li s0, 0     \n\
    c.beqz s0, end \n\
    c.li a0, 7     \n\
end:               \n\
    li a7, 93      \n\
    ecall          \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_NONE);
    TEST_ASSERT_EQUAL_UINT32(0U, g_regs[REG_A0]);
}

void test_rvc_runtime_c_bnez(void) {
    build_and_run(
        "\
.globl _start      \n\
_start:            \n\
    c.li s0, 1     \n\
    c.bnez s0, end \n\
    c.li a0, 7     \n\
end:               \n\
    li a7, 93      \n\
    ecall          \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_NONE);
    TEST_ASSERT_EQUAL_UINT32(0U, g_regs[REG_A0]);
}

void test_rvc_runtime_c_sub(void) {
    build_and_run(
        "\
.globl _start    \n\
_start:          \n\
    c.li s0, 10  \n\
    c.li s1, 3   \n\
    c.sub s0, s1 \n\
    c.mv  a0, s0 \n\
    li a7, 93    \n\
    ecall        \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_NONE);
    TEST_ASSERT_EQUAL_UINT32(7U, g_regs[REG_A0]);
}

void test_rvc_runtime_c_slli(void) {
    build_and_run(
        "\
.globl _start    \n\
_start:          \n\
    c.li   a0, 1 \n\
    c.slli a0, 4 \n\
    li a7, 93    \n\
    ecall        \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_NONE);
    TEST_ASSERT_EQUAL_UINT32(16U, g_regs[REG_A0]);
}

void test_rvc_runtime_c_jalr(void) {
    build_and_run(
        "\
fn:             \n\
    li a0, 1234 \n\
    c.jr ra     \n\
.globl _start   \n\
_start:         \n\
    la a1, fn   \n\
    c.jalr a1   \n\
    li a7, 93   \n\
    ecall       \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_NONE);
    TEST_ASSERT_EQUAL_UINT32(1234, g_regs[REG_A0]);
}

void test_rvc_runtime_lwsp_swsp(void) {
    build_and_run(
        "\
.globl _start          \n\
_start:                \n\
    c.li a0, 1         \n\
    c.jal fun          \n\
    li a7, 93          \n\
    ecall              \n\
fun:                   \n\
    c.addi16sp sp, -16 \n\
    c.swsp a0, 0(sp)   \n\
    li a0, 123456      \n\
    c.lwsp a0, 0(sp)   \n\
    c.addi16sp sp, 16  \n\
    c.jr ra            \n\
");
    TEST_ASSERT_EQUAL(g_runtime_error_type, ERROR_NONE);
    TEST_ASSERT_EQUAL_UINT32(1, g_regs[REG_A0]);
}
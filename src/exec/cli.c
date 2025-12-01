#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ezld/include/ezld/linker.h"
#include "ezld/include/ezld/runtime.h"
#include "ares/callsan.h"
#include "ares/core.h"
#include "ares/elf.h"
#include "ares/emulate.h"
#include "ares/util.h"
#include "vendor/commander.h"

// Type of command handler functions (c_*)
typedef void (*cmd_func_t)(void);

// Default output paths, changed by opt_o
static char *g_obj_out = "a.o";
static char *g_exec_out = "a.out";
static bool g_out_changed = false;

// Copies of argc, argv from main
static char **g_argv;
static int g_argc;

// The next argument in the command, set by opt_*
// for commands c_*
static char *g_next_arg = NULL;
static cmd_func_t g_command = NULL;

// These are non-command arguments
// Set in main
static const char **g_cmd_args;
static int g_cmd_args_len;

// Flags
// Set by variout opt_* like --sanitize, --fuzz
static bool g_flg_callsan = false;

// The file text, used as backing storage by all global strings
// this simplifies lifetime management significantly
static char *g_txt;

// SETUP FUNCTIONS

static void update_argument(const char *arg) {
    if (g_command) {
        fprintf(stderr, "only one command is allowed\n");
        exit(-1);
    }

    if (arg) {
        g_next_arg = strdup(arg);
        ARES_CHECK_OOM(g_next_arg);
    }
}

// UTILITY FUNCTIONS

static void emulate_safe(void) {
    while (!g_exited) {
        emulate();

        switch (g_runtime_error_type) {
            case ERROR_NONE:
                break;

            case ERROR_FETCH:
                fprintf(stderr,
                        "emulator: fetch error at pc=0x%08x on addr=0x%08x\n",
                        g_pc, g_runtime_error_params[0]);
                return;

            case ERROR_LOAD:
                fprintf(stderr,
                        "emulator: load error at pc=0x%08x on addr=0x%08x\n",
                        g_pc, g_runtime_error_params[0]);
                return;

            case ERROR_STORE:
                fprintf(stderr,
                        "emulator: store error at pc=0x%08x on addr=0x%08x\n",
                        g_pc, g_runtime_error_params[0]);
                return;

            case ERROR_UNHANDLED_INSN:
                fprintf(stderr,
                        "emulator: unhandled instruction at pc=0x%08x\n", g_pc);
                goto err;

            case ERROR_CALLSAN_CANTREAD:
                fprintf(stderr,
                        "callsan: attempt to read from uninitialized register "
                        "%s at pc=0x%08x. Check the calling convention!\n",
                        REGISTER_NAMES[g_runtime_error_params[0]], g_pc);
                goto err;

            case ERROR_CALLSAN_NOT_SAVED:
                fprintf(stderr,
                        "callsan: attempt to write callee-saved register %s at "
                        "pc=0x%08x without saving it first. Check the calling "
                        "convention!\n",
                        REGISTER_NAMES[g_runtime_error_params[0]], g_pc);
                goto err;

            case ERROR_CALLSAN_RA_MISMATCH:
                fprintf(
                    stderr,
                    "callsan: attempt to return from non-leaf function without "
                    "restoring ra register at pc=0x%08x. Check the calling "
                    "convention!\n",
                    g_pc);
                goto err;

            case ERROR_CALLSAN_SP_MISMATCH:
                fprintf(
                    stderr,
                    "callsan: attempt to return from function with wrong stack "
                    "pointer value at pc=0x%08x\n",
                    g_pc);
                goto err;

            case ERROR_CALLSAN_RET_EMPTY:
                fprintf(
                    stderr,
                    "callsan: attempt to return without a call at pc=0x%08x\n",
                    g_pc);
                goto err;

            case ERROR_CALLSAN_LOAD_STACK:
                fprintf(stderr,
                        "callsan: attempt to read at pc=0x%08x from stack "
                        "address 0x%08x, which hasn't been written to in the "
                        "current function\n",
                        g_pc, g_runtime_error_params[0]);
                goto err;

            default:
                fprintf(stderr, "emulator: unhandled error at pc=0x%08x\n",
                        g_pc);

                return;
        }
    }

    return;

err:
    if (!g_flg_callsan) {
        return;
    }

    puts("");
    puts("===================== ARES SANITIZER ERROR");
    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_shadow_stack); i++) {
        ShadowStackEnt *ent = ARES_ARRAY_GET(&g_shadow_stack, i);
        fprintf(stderr, "\t#%zu pc=0x%08x sp=0x%08x ", i, ent->pc, ent->sp);
        LabelData *label;
        u32 off;
        if (pc_to_label_r(ent->pc, &label, &off)) {
            // TODO: size_t can be > INT_MAX though I think no-one will ever
            // write a string longer than 2.1B chars
            fprintf(stderr, "(at %.*s+0x%x", (int)label->len, label->txt, off);
            size_t line_idx = (ent->pc - TEXT_BASE) / 4;

            if (line_idx < ARES_ARRAY_LEN(&g_text_by_linenum)) {
                u32 linenum = *ARES_ARRAY_GET(&g_text_by_linenum, line_idx);
                fprintf(stderr, ", line %u)", linenum);
            } else {
                fprintf(stderr, ")");
            }
        }
        puts("");
    }
    puts("");
    for (size_t i = 0; i < 32; i += 4) {
        for (size_t j = 0; j < 4; j++) {
            fprintf(stderr, "x%zu: ", i + j);
            if (i + j < 10) {
                fprintf(stderr, " ");
            }
            fprintf(stderr, "0x%08x    ", g_regs[i + j]);
        }
        puts("");
    }
}

static void assemble_from_file(const char *src_path, bool allow_externs) {
    FILE *f = fopen(src_path, "r");

    if (!f) {
        g_error = "assembler: could not open input file";
        fprintf(stderr, "%s\n", g_error);
        return;
    }

    fseek(f, 0, SEEK_END);
    size_t s = ftell(f);
    rewind(f);
    g_txt = malloc(s);
    ARES_CHECK_OOM(g_txt);
    fread(g_txt, s, 1, f);
    fclose(f);

    assemble(g_txt, s, allow_externs);

    if (g_error) {
        fprintf(stderr, "assembler: line %u %s\n", g_error_line, g_error);
    }
}

// COMMANDS

static void c_build(void) {
    FILE *out = NULL;
    assemble_from_file(g_next_arg, false);

    if (g_error) goto exit;

    void *elf_contents = NULL;
    size_t elf_sz = 0;
    char *error = NULL;

    if (!elf_emit_exec(&elf_contents, &elf_sz, &error)) {
        fprintf(stderr, "linker: %s\n", error);
        goto exit;
    }

    out = fopen(g_exec_out, "wb");

    if (!out) {
        fprintf(stderr, "linker: could not open output file\n");
        goto exit;
    }

    fwrite(elf_contents, elf_sz, 1, out);

exit:
    if (out) fclose(out);
    if (g_txt) {
        free(g_txt);
        g_txt = NULL;
    }
    return;
}

static void c_run(void) {
    FILE *elf = fopen(g_next_arg, "rb");
    u8 *elf_contents = NULL;
    char *error = NULL;

    if (!elf) {
        fprintf(stderr, "loader: could not open input file\n");
        goto exit;
    }

    fseek(elf, 0, SEEK_END);
    size_t sz = ftell(elf);
    rewind(elf);

    elf_contents = malloc(sz);
    ARES_CHECK_OOM(elf_contents);

    fread(elf_contents, sz, 1, elf);

    ARES_CHECK_CALL(elf_load(elf_contents, sz, &error), exit);

    emulate_safe();

exit:
    if (error) fprintf(stderr, "loader: %s\n", error);
    if (elf) fclose(elf);
    if (elf_contents) free(elf_contents);
}

static void c_emulate(void) {
    assemble_from_file(g_next_arg, false);
    if (g_error) goto exit;

    emulate_safe();

exit:
    if (g_txt) {
        free(g_txt);
        g_txt = NULL;
    }
}

static void c_readelf(void) {
    FILE *elf = fopen(g_next_arg, "rb");
    char *error = NULL;
    u8 *elf_contents = NULL;

    if (!elf) {
        error = "could not open input file";
        goto exit;
    }

    fseek(elf, 0, SEEK_END);
    size_t sz = ftell(elf);
    rewind(elf);

    elf_contents = malloc(sz);
    ARES_CHECK_OOM(elf_contents);

    fread(elf_contents, sz, 1, elf);
    ReadElfResult readelf = {0};
    ARES_CHECK_CALL(elf_read(elf_contents, sz, &readelf, &error), exit);

    printf(" %-35s:", "Magic");
    for (size_t i = 0; i < 8; i++) {
        printf(" %02x", readelf.magic8[i]);
    }
    printf("\n");

    printf(" %-35s: %s\n", "Class", readelf.class);
    printf(" %-35s: %s\n", "Endianness", readelf.endianness);
    printf(" %-35s: %u\n", "Version", readelf.ehdr->ehdr_ver);
    printf(" %-35s: %s\n", "OS/ABI", readelf.abi);
    printf(" %-35s: %s\n", "Type", readelf.type);
    printf(" %-35s: %s\n", "Architecture", readelf.architecture);
    printf(" %-35s: 0x%08x\n", "Entry point", readelf.ehdr->entry);
    printf(" %-35s: %u (bytes into file)\n", "Start of program headers",
           readelf.ehdr->phdrs_off);
    printf(" %-35s: %u (bytes into file)\n", "Start of section headers",
           readelf.ehdr->shdrs_off);
    printf(" %-35s: 0x%x\n", "Flags", readelf.ehdr->flags);
    printf(" %-35s: %u (bytes)\n", "Size of ELF header", readelf.ehdr->ehdr_sz);
    printf(" %-35s: %u (bytes)\n", "Size of each program header",
           readelf.ehdr->phent_sz);
    printf(" %-35s: %u\n", "Number of program headers",
           readelf.ehdr->phent_num);
    printf(" %-35s: %u (bytes)\n", "Size of each section header",
           readelf.ehdr->shent_sz);
    printf(" %-35s: %u\n", "Number of section headers",
           readelf.ehdr->shent_num);
    printf(" %-35s: %u\n", "Section header string table index",
           readelf.ehdr->shdr_str_idx);
    printf("\n");

    printf("Section headers:\n");
    printf(" [Nr] %-17s %-15s %-10s %-10s %-10s %-5s %-5s\n", "Name", "Type",
           "Address", "Offset", "Size", "Flags", "Align");

    for (u32 i = 0; i < readelf.ehdr->shent_num; i++) {
        ReadElfSection *sec = &readelf.shdrs[i];
        // clang-format off
        printf(" [%2u] %-17s %-15s 0x%08x 0x%08x 0x%08x %5s %5u\n",
                i, sec->name, sec->type, sec->shdr->virt_addr,
               sec->shdr->off, sec->shdr->mem_sz, sec->flags, sec->shdr->align);
        // clang-format on
    }
    printf("\n");

    printf("Program headers:\n");
    printf(" %-14s %-10s %-15s %-16s %-10s %-5s %-5s\n", "Type", "Offset",
           "Virtual Address", "Physical Address", "Size", "Flags", "Align");
    for (u32 i = 0; i < readelf.ehdr->phent_num; i++) {
        ReadElfSegment *seg = &readelf.phdrs[i];
        // clang-format off
        printf(" %-14s 0x%08x 0x%08x      0x%08x       0x%08x %5s %5u\n",
                seg->type, seg->phdr->off, seg->phdr->virt_addr, seg->phdr->phys_addr,
                seg->phdr->mem_sz, seg->flags, seg->phdr->align);
        // clang-format on
    }
    printf("\n");

exit:
    if (error) fprintf(stderr, "readelf: %s\n", error);
    if (elf) fclose(elf);
    if (elf_contents) free(elf_contents);
}

static void c_assemble(void) {
    FILE *out = NULL;
    assemble_from_file(g_next_arg, true);
    if (g_error) goto exit;

    void *elf_contents = NULL;
    size_t elf_sz = 0;
    char *error = NULL;

    if (!elf_emit_obj(&elf_contents, &elf_sz, &error)) {
        fprintf(stderr, "assembler: %s\n", error);
        goto exit;
    }

    out = fopen(g_obj_out, "wb");

    if (!out) {
        fprintf(stderr, "assembelr: could not open output file\n");
        return;
    }

    fwrite(elf_contents, elf_sz, 1, out);

exit:
    if (out) fclose(out);
    if (g_txt) {
        free(g_txt);
        g_txt = NULL;
    }
}

static void c_link(void) {
    const char *fake_argv[] = {"linker", NULL};
    ezld_runtime_init(1, fake_argv);
    ezld_config_t cfg = {0};

    cfg.cfg_entrysym = "_start";
    cfg.cfg_outpath = g_exec_out;
    cfg.cfg_segalign = 0x1000;
    ezld_array_init(cfg.cfg_objpaths);
    ezld_array_init(cfg.cfg_sections);
    *ezld_array_push(cfg.cfg_sections) =
        (ezld_sec_cfg_t){.sc_name = ".text", .sc_vaddr = TEXT_BASE};
    *ezld_array_push(cfg.cfg_sections) =
        (ezld_sec_cfg_t){.sc_name = ".data", .sc_vaddr = DATA_BASE};

    for (int i = 0; i < g_cmd_args_len; i++) {
        const char *filpath = g_cmd_args[i];
        *ezld_array_push(cfg.cfg_objpaths) = filpath;
    }

    ezld_link(cfg);
    ezld_array_free(cfg.cfg_objpaths);
    ezld_array_free(cfg.cfg_sections);
}

static void c_hexdump() {
    FILE *file = fopen(g_next_arg, "rb");

    if (!file) {
        fprintf(stderr, "hexdump: could not open file\n");
        return;
    }

    u8 bytes[16];
    size_t bytes_read = 0;
    u32 off = 0;
    printf("[ Offset ]    %8s %8s %8s %8s\n", "[0 - 3]", "[4 - 7]", "[8 - 11]",
           "[12 - 15]");
    while ((bytes_read = fread(bytes, 1, 16, file))) {
        printf("[%08x]    ", off);
        for (size_t i = 0; i < bytes_read; i += 4) {
            for (size_t j = 0; j < 4 && i + j < bytes_read; j++) {
                printf("%02x", bytes[i + j]);
            }
            printf(" ");
        }
        printf("\n");
        off += bytes_read;
    }
    fclose(file);
}

static void c_ascii(void) {
    FILE *file = fopen(g_next_arg, "rb");

    if (!file) {
        fprintf(stderr, "ascii: could not open file\n");
        return;
    }

    char bytes[16];
    size_t bytes_read = 0;
    u32 off = 0;
    printf(
        "[ Offset ]    +00 +01 +02 +03 +04 +05 +06 +07 +08 +09 +10 +11 +12 "
        "+13 +14 +15\n");

    while ((bytes_read = fread(bytes, 1, 16, file))) {
        printf("[%08x]    ", off);
        for (size_t i = 0; i < bytes_read; i++) {
            unsigned char c = bytes[i];

            printf(" ");
            switch (c) {
                case 0:
                    printf("\\0");
                    break;

                case '\n':
                    printf("\\n");
                    break;

                case '\r':
                    printf("\\r");
                    break;

                case '\t':
                    printf("\\t");
                    break;

                case '\a':
                    printf("\\a");
                    break;

                case '\b':
                    printf("\\b");
                    break;

                default:
                    if (c >= 32 && c < 127) {
                        printf(" %c", c);
                    } else {
                        printf("%02x", c);
                    }
                    break;
            }
            printf(" ");
        }
        printf("\n");
        off += bytes_read;
    }
    fclose(file);
}

// OPTIONS

static void opt_assemble(command_t *self) {
    update_argument(self->arg);
    g_command = c_assemble;
}

static void opt_build(command_t *self) {
    update_argument(self->arg);
    g_command = c_build;
}

static void opt_run(command_t *self) {
    update_argument(self->arg);
    g_command = c_run;
}

static void opt_emulate(command_t *self) {
    update_argument(self->arg);
    g_command = c_emulate;
}

static void opt_readelf(command_t *self) {
    update_argument(self->arg);
    g_command = c_readelf;
}

static void opt_o(command_t *self) {
    g_obj_out = malloc(strlen(self->arg) + 1);
    ARES_CHECK_OOM(g_obj_out);
    strcpy(g_obj_out, self->arg);
    g_exec_out = g_obj_out;
    g_out_changed = true;
}

static void opt_hexdump(command_t *self) {
    update_argument(self->arg);
    g_command = c_hexdump;
}

static void opt_link(command_t *self) {
    update_argument(self->arg);
    g_command = c_link;
}

static void opt_ascii(command_t *self) {
    update_argument(self->arg);
    g_command = c_ascii;
}

static void opt_sanitize(command_t *self) {
    g_flg_callsan = true;
    callsan_init();
}

int main(int argc, char **argv) {
    atexit(free_runtime);
    g_argc = argc;
    g_argv = argv;

    command_t cmd;
    // TODO: place real version number
    command_init(&cmd, argv[0], "0.0.1");

    command_option(&cmd, "-a", "--assemble <file>",
                   "assemble an RV32 assembly file and output an ELF32 "
                   "relocatable object file",
                   opt_assemble);
    command_option(&cmd, "-b", "--build <file>",
                   "assemble an RV32 assembly file"
                   " and output an ELF32 executable",
                   opt_build);
    command_option(&cmd, "-r", "--run <file>", "run an ELF32 executable",
                   opt_run);
    command_option(&cmd, "-e", "--emulate <file>",
                   "assemble and run an RV32 assembly file", opt_emulate);
    command_option(&cmd, "-i", "--readelf <file>",
                   "show information about ELF file", opt_readelf);
    command_option(&cmd, "-x", "--hexdump <file>", "perform hexdump of file",
                   opt_hexdump);
    command_option(&cmd, "-c", "--ascii <file>", "perform ascii dump of file",
                   opt_ascii);
    command_option(&cmd, "-l", "--link", "link object files using ezld linker",
                   opt_link);
    command_option(&cmd, "-o", "--output <file>", "choose output file name",
                   opt_o);
    command_option(&cmd, "-s", "--sanitize",
                   "enable ares sanitizers (callsan)", opt_sanitize);
    command_parse(&cmd, argc, argv);
    g_cmd_args = (const char **)cmd.argv;
    g_cmd_args_len = cmd.argc;

    if (1 == argc || !g_command) {
        command_help(&cmd);
        command_free(&cmd);
        return EXIT_FAILURE;
    }

    g_command();
    free((void *)g_next_arg);
    if (g_out_changed) {
        free((void *)g_obj_out);
    }
    command_free(&cmd);
    return EXIT_SUCCESS;
}

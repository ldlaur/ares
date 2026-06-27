#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ares/callsan.h"
#include "ares/core.h"
#include "ares/elf.h"
#include "ares/emulate.h"
#include "ares/util.h"

// UTILITY FUNCTIONS

static void emulate_safe(bool use_callsan) {
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

            case ERROR_INVALID_ECALL:
                fprintf(stderr, "emulator: unhandled ecall %d at pc=0x%08x\n",
                        g_runtime_error_params[0], g_pc);
                goto err;

            default:
                fprintf(stderr, "emulator: unhandled error at pc=0x%08x\n",
                        g_pc);

                return;
        }
    }

    return;

err:
    if (!use_callsan) return;

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

            if (line_idx < ARES_ARRAY_LEN(&g_text->by_linenum)) {
                u32 linenum = *ARES_ARRAY_GET(&g_text->by_linenum, line_idx);
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
            if (i + j < 10) fprintf(stderr, " ");
            fprintf(stderr, "0x%08x    ", g_regs[i + j]);
        }
        puts("");
    }
}

static char *assemble_from_file(const char *src_path, bool allow_externs) {
    FILE *f = fopen(src_path, "r");

    if (!f) {
        g_error = "assembler: could not open input file";
        fprintf(stderr, "%s\n", g_error);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    size_t s = ftell(f);
    rewind(f);
    char *text = malloc(s);
    ARES_CHECK_OOM(text);
    fread(text, s, 1, f);
    fclose(f);

    assemble(text, s, allow_externs);

    if (g_error) {
        fprintf(stderr, "assembler: line %u %s\n", g_error_line, g_error);
    }

    return text;
}

// COMMANDS

static void run_elf(const char *elf_path, bool use_callsan) {
    FILE *elf = fopen(elf_path, "rb");
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

    emulate_safe(use_callsan);

exit:
    if (error) fprintf(stderr, "loader: %s\n", error);
    if (elf) fclose(elf);
    free(elf_contents);
}

static void emulate_from_source(const char *src_path, bool use_callsan) {
    char *text = assemble_from_file(src_path, false);
    if (!g_error) emulate_safe(use_callsan);
    free(text);
}

static void readelf(const char *elf_path) {
    FILE *elf = fopen(elf_path, "rb");
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

    printf(" %-35s: %s\n", "Class", readelf.clazz);
    printf(" %-35s: %s\n", "Endianness", readelf.endianness);
    printf(" %-35s: %u\n", "Version", readelf.ehdr->e_ident[EI_VERSION]);
    printf(" %-35s: %s\n", "OS/ABI", readelf.abi);
    printf(" %-35s: %s\n", "Type", readelf.type);
    printf(" %-35s: %s\n", "Architecture", readelf.architecture);
    printf(" %-35s: 0x%08x\n", "Entry point", readelf.ehdr->e_entry);
    printf(" %-35s: %u (bytes into file)\n", "Start of program headers",
           readelf.ehdr->e_phoff);
    printf(" %-35s: %u (bytes into file)\n", "Start of section headers",
           readelf.ehdr->e_shoff);
    printf(" %-35s: 0x%x\n", "Flags", readelf.ehdr->e_flags);
    printf(" %-35s: %u (bytes)\n", "Size of ELF header",
           readelf.ehdr->e_ehsize);
    printf(" %-35s: %u (bytes)\n", "Size of each program header",
           readelf.ehdr->e_phentsize);
    printf(" %-35s: %u\n", "Number of program headers", readelf.ehdr->e_phnum);
    printf(" %-35s: %u (bytes)\n", "Size of each section header",
           readelf.ehdr->e_shentsize);
    printf(" %-35s: %u\n", "Number of section headers", readelf.ehdr->e_shnum);
    printf(" %-35s: %u\n", "Section header string table index",
           readelf.ehdr->e_shstrndx);
    printf("\n");

    printf("Section headers:\n");
    printf(" [Nr] %-17s %-15s %-10s %-10s %-10s %-5s %-5s\n", "Name", "Type",
           "Address", "Offset", "Size", "Flags", "Align");

    for (u32 i = 0; i < readelf.ehdr->e_shnum; i++) {
        ReadElfSection *sec = readelf.shdrs + i;
        // clang-format off
        printf(" [%2u] %-17s %-15s 0x%08x 0x%08x 0x%08x %5s %5u\n",
                i, sec->name, sec->type, sec->shdr->sh_addr,
               sec->shdr->sh_offset, sec->shdr->sh_size, sec->flags,
               sec->shdr->sh_addralign);
        // clang-format on
    }
    printf("\n");

    printf("Program headers:\n");
    printf(" %-14s %-10s %-15s %-16s %-10s %-5s %-5s\n", "Type", "Offset",
           "Virtual Address", "Physical Address", "Size", "Flags", "Align");
    for (u32 i = 0; i < readelf.ehdr->e_phnum; i++) {
        ReadElfSegment *seg = readelf.phdrs + i;
        // clang-format off
        printf(" %-14s 0x%08x 0x%08x      0x%08x       0x%08x %5s %5u\n",
                seg->type, seg->phdr->p_offset, seg->phdr->p_vaddr, seg->phdr->p_paddr,
                seg->phdr->p_memsz, seg->flags, seg->phdr->p_align);
        // clang-format on
    }
    printf("\n");

exit:
    if (error) fprintf(stderr, "readelf: %s\n", error);
    if (elf) fclose(elf);
    free(elf_contents);
    free(readelf.phdrs);
    free(readelf.shdrs);
}

static void hexdump(const char *file_path) {
    FILE *file = fopen(file_path, "rb");

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

static void asciidump(const char *file_path) {
    FILE *file = fopen(file_path, "rb");

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

int main(int argc, const char *const *const argv) {
    if (argc < 2) {
        fprintf(stderr, "ares: invalid commandline, try 'help'\n");
        return EXIT_FAILURE;
    }

    atexit(free_runtime);
    const char *const command = argv[1];

    if (strcmp(command, "help") == 0) {
        printf("Usage: %s <command> [--callsan] <file>\n\n", argv[0]);
        printf("Commands:\n");
        printf(
            "  check      <file>   Check a source file for assembly language "
            "errors\n");
        printf(
            "  emulate    <file>   Emulate from source (supports --callsan)\n");
        printf("  runelf     <file>   Run an ELF file (supports --callsan)\n");
        printf("  readelf    <file>   Parse and display ELF structure\n");
        printf("  hexdump    <file>   Print a hex dump of a file\n");
        printf("  asciidump  <file>   Print an ASCII dump of a file\n");
        printf("  help                Show this help message\n");
        return EXIT_SUCCESS;
    }

    bool use_callsan = false;
    const char *file_path = NULL;

    // Parse arguments regardless of order
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--callsan") == 0) use_callsan = true;
        else if (!file_path) file_path = argv[i];
        else {
            fprintf(stderr, "ares: unexpected argument '%s'\n", argv[i]);
            return EXIT_FAILURE;
        }
    }

    if (!file_path) {
        fprintf(stderr, "ares: missing file path for command '%s'\n", command);
        return EXIT_FAILURE;
    }

    // Route to appropriate function
    if (strcmp(command, "check") == 0) {
        char *text = assemble_from_file(file_path, false);
        free(text);
    } else if (strcmp(command, "emulate") == 0) {
        emulate_from_source(file_path, use_callsan);
    } else if (strcmp(command, "runelf") == 0) {
        run_elf(file_path, use_callsan);
    } else if (strcmp(command, "readelf") == 0) {
        readelf(file_path);
    } else if (strcmp(command, "hexdump") == 0) {
        hexdump(file_path);
    } else if (strcmp(command, "asciidump") == 0) {
        asciidump(file_path);
    } else {
        fprintf(stderr, "ares: unknown command '%s'\n", command);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

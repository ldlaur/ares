#include "ares/elf.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ares/core.h"
#include "ares/emulate.h"
#include "ares/util.h"

// TODO: if the host machine and RISC-V have mismatched byte orders (i.e., the
// host is big endian, as is the case for SPARC and other defunct
// architectures), then the output object file will be broken. This endianness
// UB is quite easy to fix, code is already present in ezld, but porting it
// takes time and it's unlikelly to ever be used

#define UNKNOWN_PROP "Unknown"

#define STRTAB_ISTR 1   // Index of .strtab in strtab
#define STRTAB_ISYM 9   // Index of .symtab in strtab
#define STRTAB_ISEC 17  // Start of section names in strtab

static inline void copy_n(void *dst, const void *src, size_t src_sz,
                          size_t *off) {
    memcpy((uint8_t *)dst + *off, src, src_sz);
    *off += src_sz;
}

static inline void copy_s(void *dst, const char *src, size_t *off) {
    size_t len = strlen(src) + 1;
    memcpy((uint8_t *)dst + *off, src, len);
    *off += len;
}

bool elf_read(u8 *elf_contents, size_t elf_contents_len, ReadElfResult *out,
              char **error) {
    if (!elf_contents) {
        *error = "null buffer";
        return false;
    }

    if (elf_contents_len < sizeof(ElfHeader)) {
        *error = "corrupt or invalid elf header";
        return false;
    }

    ReadElfSegment *readable_phdrs = NULL;
    ReadElfSection *readable_shdrs = NULL;
    ElfHeader *e_header = (ElfHeader *)elf_contents;

    if (e_header->magic[0] != 0x7F || e_header->magic[1] != 'E' ||
        e_header->magic[2] != 'L' || e_header->magic[3] != 'F') {
        *error = "not an elf file";
        return false;
    }

    out->ehdr = e_header;
    out->magic8 = elf_contents;

    // Print file class
    if (e_header->bits == 1) {
        out->class = "ELF32";
    } else if (e_header->bits == 2) {
        out->class =
            "ELF64 (WARNING: Corrupt content ahead, format not supported)";
    } else {
        out->class = UNKNOWN_PROP;
    }

    // Print file endianness
    if (e_header->endianness == 1) {
        out->endianness = "Little endian";
    } else if (e_header->endianness == 2) {
        out->endianness = "Big endian";
    } else {
        out->endianness = UNKNOWN_PROP;
    }

    // Print OS/ABI
    if (e_header->abi == 0) {
        out->abi = "UNIX - System V";
    } else {
        out->abi = UNKNOWN_PROP;
    }

    // Print ELF type
    if (e_header->type == 1) {
        out->type = "Relocatable";
    } else if (e_header->type == 2) {
        out->type = "Executable";
    } else if (e_header->type == 3) {
        out->type = "Shared";
    } else if (e_header->type == 4) {
        out->type = "Core";
    } else {
        out->type = UNKNOWN_PROP;
    }

    // Print architecture
    if (e_header->isa == 0xF3) {
        out->architecture = "RISC-V";
    } else if (e_header->isa == 0x3E) {
        out->architecture = "x86-64 (x64, AMD/Intel 64 bit)";
    } else if (e_header->isa == 0xB7) {
        out->architecture = "AArch64 (ARM64)";
    } else {
        out->architecture = UNKNOWN_PROP;
    }

    if (e_header->phdrs_off >= elf_contents_len ||
        e_header->phdrs_off + (e_header->phent_sz * e_header->phent_num) >
            elf_contents_len) {
        *error = "program headers offset exceeds buffer size";
        goto fail;
    }

    ElfProgramHeader *phdrs =
        (ElfProgramHeader *)(elf_contents + e_header->phdrs_off);
    readable_phdrs = malloc(sizeof(ReadElfSegment) * e_header->phent_num);
    ARES_CHECK_OOM(readable_phdrs);

    for (u32 i = 0; i < e_header->phent_num; i++) {
        ElfProgramHeader *phdr = &phdrs[i];
        ReadElfSegment *readable = &readable_phdrs[i];
        size_t flags_idx = 0;

        readable->phdr = phdr;

        if (phdr->flags & 0b100) {
            readable->flags[flags_idx++] = 'R';
        }

        if (phdr->flags & 0b010) {
            readable->flags[flags_idx++] = 'W';
        }

        if (phdr->flags & 0b001) {
            readable->flags[flags_idx++] = 'X';
        }

        readable->flags[flags_idx] = 0;

        switch (phdr->type) {
            case PT_LOAD:
                readable->type = "LOAD";
                break;

            case PT_NULL:
                readable->type = "NULL";
                break;

            case PT_DYNAMIC:
                readable->type = "DYNAMIC";
                break;

            case PT_INTERP:
                readable->type = "INTERP";
                break;

            case PT_NOTE:
                readable->type = "NOTE";
                break;

            default:
                readable->type = UNKNOWN_PROP;
                break;
        }
    }

    if (e_header->shdrs_off >= elf_contents_len ||
        e_header->shdrs_off + (e_header->shent_sz * e_header->shent_num) >
            elf_contents_len) {
        *error = "section headers offset exceeds buffer size";
        goto fail;
    }

    ElfSectionHeader *shdrs =
        (ElfSectionHeader *)(elf_contents + e_header->shdrs_off);
    readable_shdrs = malloc(sizeof(ReadElfSection) * e_header->shent_num);
    ARES_CHECK_OOM(readable_shdrs);

    ElfSectionHeader *str_sh = &shdrs[e_header->shdr_str_idx];
    char *str_tab = (char *)(elf_contents + str_sh->off);
    u32 str_tab_sz = str_sh->mem_sz;

    for (u32 i = 0; i < e_header->shent_num; i++) {
        ElfSectionHeader *shdr = &shdrs[i];
        ReadElfSection *readable = &readable_shdrs[i];
        size_t flags_idx = 0;

        if (shdr->name_off >= str_tab_sz) {
            *error = "section name out of bounds of string table section";
            goto fail;
        }

        readable->shdr = shdr;

        if (shdr->flags & SHF_WRITE) {
            readable->flags[flags_idx++] = 'W';
        }

        if (shdr->flags & SHF_ALLOC) {
            readable->flags[flags_idx++] = 'A';
        }

        if (shdr->flags & SHF_STRINGS) {
            readable->flags[flags_idx++] = 'S';
        }

        if (shdr->flags & SHF_EXECINSTR) {
            readable->flags[flags_idx++] = 'X';
        }

        readable->flags[flags_idx] = 0;
        readable->name = &str_tab[shdr->name_off];

        switch (shdr->type) {
            case SHT_NULL:
                readable->type = "NULL";
                break;

            case SHT_PROGBITS:
                readable->type = "PROGBITS";
                break;

            case SHT_SYMTAB:
                readable->type = "SYMTAB";
                break;

            case SHT_STRTAB:
                readable->type = "STRTAB";
                break;

            default:
                readable->type = UNKNOWN_PROP;
                break;
        }
    }

    out->phdrs = readable_phdrs;
    out->shdrs = readable_shdrs;
    return true;

fail:
    free(readable_phdrs);
    free(readable_shdrs);
    return false;
}

bool elf_load(u8 *elf_contents, size_t elf_len, char **error) {
    if (!elf_contents) {
        *error = "null buffer";
        return false;
    }

    if (elf_len < sizeof(ElfHeader)) {
        *error = "corrupt or invalid elf header";
        return false;
    }

    ElfHeader *e_header = (ElfHeader *)elf_contents;

    if (e_header->magic[0] != 0x7F || e_header->magic[1] != 'E' ||
        e_header->magic[2] != 'L' || e_header->magic[3] != 'F') {
        *error = "not an elf file";
        return false;
    }

    if (e_header->bits != 1) {
        *error = "unsupported elf variant (only elf32 is supported)";
        return false;
    }

    if (e_header->isa != 0xF3) {
        *error = "unsupported architecture (only risc-v is supported)";
        return false;
    }

    if (e_header->type != 2) {
        *error = "not an elf executable";
        return false;
    }

    // ElfProgramHeader *phdrs =
    //     (ElfProgramHeader *)(elf_contents + e_header->phdrs_off);
    ElfSectionHeader *shdrs =
        (ElfSectionHeader *)(elf_contents + e_header->shdrs_off);

    ElfSectionHeader *str_tab_shdr = &shdrs[e_header->shdr_str_idx];
    char *str_tab = (char *)(elf_contents + str_tab_shdr->off);
    u32 str_tab_len = str_tab_shdr->mem_sz;

    for (u32 i = 0; i < e_header->shent_num; i++) {
        ElfSectionHeader *s_hdr = &shdrs[i];
        if (!(s_hdr->flags & SHF_ALLOC)) {
            continue;
        }

        Section *s = calloc(1, sizeof(Section));
        ARES_CHECK_OOM(s);
        s->read = true;
        s->align = s_hdr->align;
        s->base = s_hdr->virt_addr;
        s->contents.cap = s->contents.len = s_hdr->mem_sz;
        s->contents.buf = calloc(1, s->contents.len);
        ARES_CHECK_OOM(s->contents.buf);
        if (s_hdr->type != SHT_NOBITS)
            memcpy(s->contents.buf, elf_contents + s_hdr->off, s->contents.len);

        s->limit = s->base + s->contents.len;

        if (s_hdr->name_off >= str_tab_len) {
            *error = "section header name offset out of range";
            free(s);
            goto fail;
        }

        s->name = str_tab + s_hdr->name_off;

        if (s_hdr->flags & SHF_WRITE) {
            s->write = true;
        }

        if (s_hdr->flags & SHF_EXECINSTR) {
            s->execute = true;
        }

        *ARES_ARRAY_PUSH(&g_sections) = s;
    }

    emulator_init();
    g_pc = e_header->entry;
    return true;

fail:
    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_sections); i++) {
        free(*ARES_ARRAY_GET(&g_sections, i));
    }
    ARES_ARRAY_FREE(&g_sections);
    return false;
}

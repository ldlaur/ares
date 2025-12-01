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

    if (0x7F != e_header->magic[0] || 'E' != e_header->magic[1] ||
        'L' != e_header->magic[2] || 'F' != e_header->magic[3]) {
        *error = "not an elf file";
        return false;
    }

    out->ehdr = e_header;
    out->magic8 = elf_contents;

    // Print file class
    if (1 == e_header->bits) {
        out->class = "ELF32";
    } else if (2 == e_header->bits) {
        out->class =
            "ELF64 (WARNING: Corrupt content ahead, format not supported)";
    } else {
        out->class = UNKNOWN_PROP;
    }

    // Print file endianness
    if (1 == e_header->endianness) {
        out->endianness = "Little endian";
    } else if (2 == e_header->endianness) {
        out->endianness = "Big endian";
    } else {
        out->endianness = UNKNOWN_PROP;
    }

    // Print OS/ABI
    if (0 == e_header->abi) {
        out->abi = "UNIX - System V";
    } else {
        out->abi = UNKNOWN_PROP;
    }

    // Print ELF type
    if (1 == e_header->type) {
        out->type = "Relocatable";
    } else if (2 == e_header->type) {
        out->type = "Executable";
    } else if (3 == e_header->type) {
        out->type = "Shared";
    } else if (4 == e_header->type) {
        out->type = "Core";
    } else {
        out->type = UNKNOWN_PROP;
    }

    // Print architecture
    if (0xF3 == e_header->isa) {
        out->architecture = "RISC-V";
    } else if (0x3E == e_header->isa) {
        out->architecture = "x86-64 (x64, AMD/Intel 64 bit)";
    } else if (0xB7 == e_header->isa) {
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

        if (0b100 & phdr->flags) {
            readable->flags[flags_idx++] = 'R';
        }

        if (0b010 & phdr->flags) {
            readable->flags[flags_idx++] = 'W';
        }

        if (0b001 & phdr->flags) {
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

        if (SHF_WRITE & shdr->flags) {
            readable->flags[flags_idx++] = 'W';
        }

        if (SHF_ALLOC & shdr->flags) {
            readable->flags[flags_idx++] = 'A';
        }

        if (SHF_STRINGS & shdr->flags) {
            readable->flags[flags_idx++] = 'S';
        }

        if (SHF_EXECINSTR & shdr->flags) {
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

// Constructs a buffer containing program headers, segments and section headers
// ORDER;
// - Program headers
// - Segments
// - Section headers
// ORDER OF SECTION HEADERS:
// - NULL section
// - Reserved sections
// - Segment-related sections (in the same order as the segments)
// - Relocation sections (in the same order as the segments)
// ADDITIONAL INFORMATION:
// This function assumes that section names are the first strings in the strtab.
// This function also assumes that name_off points to a value > 0.
// phdrs_start, shdrs_start, name_off are all byte offsets.
// This functions also assumes that the names of relocation sections follow
// those of the relative section withing the string table. E.g., .rela.text
// comes immediately after .text NOTE: This function changes elf.shidx in each
// physical section in g_sections with len > 0
static bool make_core(u8 **out, size_t *out_sz, size_t *name_off,
                      size_t *phdrs_start, size_t *shdrs_start, size_t *phnum,
                      size_t *shnum, size_t *reloc_idx, size_t *reloc_num,
                      size_t file_off, size_t rsv_shdrs, size_t symtab_idx,
                      bool use_phdrs, bool use_shdrs, char **error) {
    size_t segments_count = 0;
    size_t segments_sz = 0;
    size_t reloc_shdrs_num = 0;
    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_sections); i++) {
        Section *s = *ARES_ARRAY_GET(&g_sections, i);
        if (s->physical && 0 != s->contents.len) {
            segments_count++;
            segments_sz += s->contents.len;

            if (0 != s->relocations.len) {
                reloc_shdrs_num++;
            }
        }
    }

    size_t sections_count = 1 + segments_count + rsv_shdrs + reloc_shdrs_num;
    size_t region_sz = segments_sz;

    if (use_phdrs) {
        region_sz += segments_count * sizeof(ElfProgramHeader);
    }

    if (use_shdrs) {
        region_sz += sections_count * sizeof(ElfSectionHeader);
    }

    u8 *region = malloc(region_sz);
    ARES_CHECK_OOM(region);

    size_t phdrs_off = 0;
    size_t segment_off = 0;

    if (use_phdrs) {
        segment_off += segments_count * sizeof(ElfProgramHeader);
    }

    size_t shdrs_off = segment_off + segments_sz;
    size_t shdrs_i = 0;

    // Create null section
    if (use_shdrs) {
        ElfSectionHeader null_s = {0};
        null_s.type = SHT_NULL;
        copy_n(region, &null_s, sizeof(null_s), &shdrs_off);
        shdrs_i++;
    }

    // Move past reserved sections
    shdrs_off += rsv_shdrs * sizeof(ElfSectionHeader);
    shdrs_i += rsv_shdrs;

    size_t reloc_off = shdrs_off + segments_count * sizeof(ElfSectionHeader);
    size_t reloc_i = 1 + rsv_shdrs + segments_count;

    // Return already known values
    if (NULL != reloc_idx) {
        *reloc_idx = reloc_i;
    }
    if (NULL != reloc_num) {
        *reloc_num = reloc_shdrs_num;
    }
    *out = region;
    *out_sz = region_sz;
    *phdrs_start = 0;
    *shdrs_start = segments_sz;
    if (use_phdrs) {
        *shdrs_start += segments_count * sizeof(ElfProgramHeader);
    }
    *phnum = segments_count;
    *shnum = sections_count;

    // Write program headers, segments, and section headers
    // RELOCATION HEADERS EXCLUDED
    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_sections); i++) {
        Section *s = *ARES_ARRAY_GET(&g_sections, i);
        if (!s->physical || 0 == s->contents.len) {
            continue;
        }

        // Ugly but avoids UB
        u32 phdr_flags = 0;
        if (s->read) {
            phdr_flags |= 0b100;
        }
        if (s->write) {
            phdr_flags |= 0b010;
        }
        if (s->execute) {
            phdr_flags |= 0b001;
        }

        ElfProgramHeader prog_header = {.type = PT_LOAD,
                                        .flags = phdr_flags,
                                        .off = segment_off + file_off,
                                        .virt_addr = s->base,
                                        .phys_addr = s->base,
                                        .file_sz = s->contents.len,
                                        .mem_sz = s->contents.len,
                                        .align = s->align};

        // Ugly but avoids UB
        u32 shdr_flags = SHF_ALLOC;
        if (s->write) {
            shdr_flags |= SHF_WRITE;
        }
        if (s->execute) {
            shdr_flags |= SHF_EXECINSTR;
        }

        ElfSectionHeader sec_header = {.name_off = *name_off,
                                       .type = SHT_PROGBITS,
                                       .flags = shdr_flags,
                                       .off = segment_off + file_off,
                                       .virt_addr = s->base,
                                       .mem_sz = s->contents.len,
                                       .align = s->align,
                                       .link = 0,
                                       .ent_sz = 0};

        if (use_phdrs) {
            copy_n(region, &prog_header, sizeof(prog_header), &phdrs_off);
        }
        copy_n(region, s->contents.buf, s->contents.len, &segment_off);
        if (use_shdrs) {
            s->elf.shidx = shdrs_i;
            *(name_off) += strlen(s->name) + 1;
            copy_n(region, &sec_header, sizeof(sec_header), &shdrs_off);
        }

        // Write relocation section header
        if (use_shdrs && 0 != s->relocations.len) {
            ElfSectionHeader reloc_shdr = {.name_off = *name_off,
                                           .type = SHT_RELA,
                                           .flags = SHF_INFO_LINK,
                                           .info = shdrs_i,
                                           .off = 0,
                                           .virt_addr = 0,
                                           .mem_sz = 0,
                                           .align = 1,
                                           .link = symtab_idx,
                                           .ent_sz = sizeof(ElfRelaEntry)};
            copy_n(region, &reloc_shdr, sizeof(reloc_shdr), &reloc_off);
            *(name_off) += strlen(".rela") + strlen(s->name) + 1;
        }

        // Tail update to index to avoid issue with relocation sections
        shdrs_i++;
    }

    return true;
fail:
    free(region);
    return false;
}

// This function makes an ELF string table
// The string table always starts with:
// \0.strtab\0.symtab\0
// Thus, the indices for .startab and .symtab are 1 and 9 respectively
// Section names start at index 17
// Then come, in this order, externs and globals (if included)
static bool make_strtab(char **out, size_t *out_sz, bool inc_externs,
                        bool inc_globs, char **error) {
    size_t base_len = strlen(".strtab") + 1 + strlen(".symtab") + 1;
    size_t strtab_sz = 1 + base_len;
    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_sections); i++) {
        Section *s = *ARES_ARRAY_GET(&g_sections, i);
        if (s->physical && 0 != s->contents.len) {
            strtab_sz += strlen(s->name) + 1;

            if (0 != s->relocations.len) {
                strtab_sz += strlen(".rela") + strlen(s->name) + 1;
            }
        }
    }
    if (inc_externs) {
        for (size_t i = 0; i < ARES_ARRAY_LEN(&g_externs); i++) {
            Extern *e = ARES_ARRAY_GET(&g_externs, i);
            strtab_sz += e->len + 1;
        }
    }
    if (inc_globs) {
        for (size_t i = 0; i < ARES_ARRAY_LEN(&g_globals); i++) {
            Global *g = ARES_ARRAY_GET(&g_globals, i);
            strtab_sz += g->len + 1;
        }
    }

    char *strtab = malloc(strtab_sz);
    ARES_CHECK_OOM(strtab);

    strtab[0] = '\0';
    size_t strtab_off = 1;

    copy_s(strtab, ".strtab", &strtab_off);
    copy_s(strtab, ".symtab", &strtab_off);
    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_sections); i++) {
        Section *s = *ARES_ARRAY_GET(&g_sections, i);
        if (s->physical && 0 != s->contents.len) {
            copy_s(strtab, s->name, &strtab_off);

            if (0 != s->relocations.len) {
                copy_s(strtab, ".rela", &strtab_off);
                strtab_off--;
                copy_s(strtab, s->name, &strtab_off);
            }
        }
    }

    if (inc_externs) {
        for (size_t i = 0; i < ARES_ARRAY_LEN(&g_externs); i++) {
            Extern *e = ARES_ARRAY_GET(&g_externs, i);
            copy_n(strtab, e->symbol, e->len, &strtab_off);
            strtab[strtab_off++] = '\0';
        }
    }

    if (inc_globs) {
        for (size_t i = 0; i < ARES_ARRAY_LEN(&g_globals); i++) {
            Global *g = ARES_ARRAY_GET(&g_globals, i);
            copy_n(strtab, g->str, g->len, &strtab_off);
            strtab[strtab_off++] = '\0';
        }
    }

    *out = strtab;
    *out_sz = strtab_sz;
    return true;

fail:
    free(strtab);
    return false;
}

static bool make_symtab(u8 **out, size_t *out_sz, size_t *ent_num,
                        size_t name_off, char **error) {
    size_t symtab_sz =
        sizeof(ElfSymtabEntry) *
        (1 + ARES_ARRAY_LEN(&g_externs) + ARES_ARRAY_LEN(&g_globals));
    ElfSymtabEntry *symtab = malloc(symtab_sz);
    ARES_CHECK_OOM(symtab);

    ElfSymtabEntry null_e = {0};
    null_e.shent_idx = SHN_UNDEF;
    symtab[0] = null_e;

    size_t symtab_i = 1;

    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_externs); i++, symtab_i++) {
        Extern *e = ARES_ARRAY_GET(&g_externs, i);
        ElfSymtabEntry *sym = &symtab[symtab_i];
        e->elf.stidx = symtab_i;
        sym->name_off = name_off;
        sym->shent_idx = SHN_UNDEF;
        sym->other = 0;
        sym->size = 0;
        sym->value = 0;
        sym->info = ELF32_ST_INFO(STB_GLOBAL, STT_NOTYPE);
        name_off += e->len + 1;
    }

    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_globals); i++, symtab_i++) {
        Global *g = ARES_ARRAY_GET(&g_globals, i);
        ElfSymtabEntry *sym = &symtab[symtab_i];
        g->elf.stidx = symtab_i;
        sym->name_off = name_off;
        sym->other = 0;
        sym->size = 0;

        u32 addr = 0;
        Section *sec = NULL;

        if (!resolve_symbol(g->str, g->len, true, &addr, &sec)) {
            *error = "symbol is declared global but never defined";
            goto fail;
        }

        sym->shent_idx = sec->elf.shidx;
        sym->value = addr - sec->base;
        sym->info = ELF32_ST_INFO(STB_GLOBAL, STT_NOTYPE);
        name_off += g->len + 1;
    }

    *out = (u8 *)symtab;
    *out_sz = symtab_sz;

    *ent_num = symtab_i;
    return true;

fail:
    free(symtab);
    return false;
}

static bool make_rela(u8 **out, size_t *out_sz, size_t file_off,
                      ElfSectionHeader *shdrs, size_t reloc_idx,
                      ElfSymtabEntry *symtab, char **error) {
    size_t rela_count = 0;
    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_sections); i++) {
        Section *s = *ARES_ARRAY_GET(&g_sections, i);
        rela_count += s->relocations.len;
    }

    ElfRelaEntry *relas = malloc(sizeof(ElfRelaEntry) * rela_count);
    ARES_CHECK_OOM(relas);

    // NOTE: this works because it assumes that section headers have been palced
    // by the make_core function. The make_core function places section headers
    // in the order they appear in the g_sections array if they contain data.
    // The same applies to .rela sections that appear in section headers
    // starting at index reloc_idx and are placed in the same order (excluding
    // sections that do not require relocations)
    size_t rel_i = 0;
    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_sections); i++) {
        Section *s = *ARES_ARRAY_GET(&g_sections, i);
        if (!s->physical || 0 == s->contents.len || 0 == s->relocations.len) {
            continue;
        }

        ElfSectionHeader *rela_shdr = &shdrs[reloc_idx];
        rela_shdr->off = file_off + rel_i * sizeof(ElfRelaEntry);
        rela_shdr->mem_sz = 0;

        for (size_t j = 0; j < s->relocations.len; j++, rel_i++) {
            Relocation *r = &s->relocations.buf[j];
            ElfRelaEntry *rela = &relas[rel_i];

            rela->offset = r->offset;
            rela->addend = r->addend;
            rela->info = ELF32_R_INFO(r->symbol->elf.stidx, r->type);
            rela_shdr->mem_sz += sizeof(ElfRelaEntry);
        }
        reloc_idx++;
    }

    *out = (u8 *)relas;
    *out_sz = rela_count * sizeof(ElfRelaEntry);
    return true;

fail:
    free(relas);
    return false;
}

bool elf_emit_exec(void **out, size_t *len, char **error) {
    char *strtab = NULL;
    u8 *core = NULL;
    u8 *elf_contents = NULL;
    size_t strtab_sz = 0;
    size_t core_sz = 0;
    size_t name_off = STRTAB_ISEC;
    size_t phdrs_start = 0;
    size_t shdrs_start = 0;
    size_t phnum = 0;
    size_t shnum = 0;

    u32 entrypoint;
    if (!resolve_symbol("_start", strlen("_start"), true, &entrypoint, NULL)) {
        *error = "unresolved reference to `_start`";
        return false;
    }

    ARES_CHECK_CALL(make_strtab(&strtab, &strtab_sz, true, true, error),
                      fail);
    ARES_CHECK_CALL(make_core(&core, &core_sz, &name_off, &phdrs_start,
                                &shdrs_start, &phnum, &shnum, NULL, NULL,
                                sizeof(ElfHeader), 1, 0, true, true, error),
                      fail);

    ElfHeader e_hdr = {
        .magic = {0x7F, 'E', 'L', 'F'},  // ELF magic
        .bits = 1,                       // 32 bits
        .endianness = 1,                 // little endian
        .ehdr_ver = 1,                   // ELF header version 1
        .abi = 0,                        // System V ABI
        .type = 2,                       // Executable
        .isa = 0xF3,                     // Arch = RISC-V
        .elf_ver = 1,                    // ELF version 1
        .entry = entrypoint,             // Program entrypoint
        .phdrs_off = sizeof(ElfHeader) +
                     phdrs_start,  // Start offset of program header tabe
        .phent_num = phnum,        // 2 program headers
        .phent_sz = sizeof(
            ElfProgramHeader),  // Size of each program header table entry
        .shdrs_off = sizeof(ElfHeader) +
                     shdrs_start,  // Start offset of section header table
        .shent_num = shnum,        // 2 sections (.text, .data)
        .shent_sz = sizeof(ElfSectionHeader),  // Size of each section header
        .ehdr_sz = sizeof(ElfHeader),          // Size of the ELF ehader
        .flags = 0,                            // Flags
        .shdr_str_idx = 1};

    ElfSectionHeader *shdrs = (ElfSectionHeader *)(core + shdrs_start);
    shdrs[1] = (ElfSectionHeader){.name_off = 1,
                                  .type = SHT_STRTAB,
                                  .flags = 0,
                                  .off = sizeof(ElfHeader) + core_sz,
                                  .virt_addr = 0,
                                  .mem_sz = strtab_sz,
                                  .align = 1,
                                  .link = 0,
                                  .ent_sz = 0};

    elf_contents = malloc(sizeof(ElfHeader) + core_sz + strtab_sz);
    ARES_CHECK_OOM(elf_contents);
    size_t elf_off = 0;

    copy_n(elf_contents, &e_hdr, sizeof(e_hdr), &elf_off);
    copy_n(elf_contents, core, core_sz, &elf_off);
    copy_n(elf_contents, strtab, strtab_sz, &elf_off);
    *out = elf_contents;
    *len = elf_off;

    free(core);
    free(strtab);
    return true;

fail:
    free(strtab);
    free(core);
    free(elf_contents);
    return false;
}

bool elf_emit_obj(void **out, size_t *len, char **error) {
    char *strtab = NULL;
    u8 *core = NULL;
    u8 *symtab = NULL;
    u8 *relas = NULL;
    u8 *elf_contents = NULL;

    size_t strtab_sz = 0;
    size_t core_sz = 0;
    size_t name_off = STRTAB_ISEC;
    size_t phdrs_start = 0;
    size_t shdrs_start = 0;
    size_t phnum = 0;
    size_t shnum = 0;
    size_t reloc_idx = 0;
    size_t reloc_num = 0;
    size_t symtab_sz = 0;
    size_t symtab_entnum = 0;
    size_t relas_sz = 0;

    ARES_CHECK_CALL(make_strtab(&strtab, &strtab_sz, true, true, error),
                      fail);
    ARES_CHECK_CALL(
        make_core(&core, &core_sz, &name_off, &phdrs_start, &shdrs_start,
                  &phnum, &shnum, &reloc_idx, &reloc_num, sizeof(ElfHeader), 2,
                  2, false, true, error),
        fail);
    ARES_CHECK_CALL(
        make_symtab(&symtab, &symtab_sz, &symtab_entnum, name_off, error),
        fail);

    ElfSectionHeader *shdrs = (ElfSectionHeader *)(core + shdrs_start);
    ARES_CHECK_CALL(
        make_rela(&relas, &relas_sz,
                  sizeof(ElfHeader) + core_sz + strtab_sz + symtab_sz, shdrs,
                  reloc_idx, (ElfSymtabEntry *)symtab, error),
        fail);

    ElfHeader e_hdr = {
        .magic = {0x7F, 'E', 'L', 'F'},  // ELF magic
        .bits = 1,                       // 32 bits
        .endianness = 1,                 // little endian
        .ehdr_ver = 1,                   // ELF header version 1
        .abi = 0,                        // System V ABI
        .type = 1,                       // Executable
        .isa = 0xF3,                     // Arch = RISC-V
        .elf_ver = 1,                    // ELF version 1
        .entry = 0,                      // Program entrypoint
        .phdrs_off = 0,                  // Start offset of program header tabe
        .phent_num = 0,                  // 2 program headers
        .phent_sz = 0,  // Size of each program header table entry
        .shdrs_off = sizeof(ElfHeader) +
                     shdrs_start,  // Start offset of section header table
        .shent_num = shnum,        // 2 sections (.text, .data)
        .shent_sz = sizeof(ElfSectionHeader),  // Size of each section header
        .ehdr_sz = sizeof(ElfHeader),          // Size of the ELF ehader
        .flags = 0,                            // Flags
        .shdr_str_idx = 1};

    shdrs[1] = (ElfSectionHeader){.name_off = STRTAB_ISTR,
                                  .type = SHT_STRTAB,
                                  .flags = 0,
                                  .off = sizeof(ElfHeader) + core_sz,
                                  .virt_addr = 0,
                                  .mem_sz = strtab_sz,
                                  .align = 1,
                                  .link = 0,
                                  .ent_sz = 0};
    shdrs[2] =
        (ElfSectionHeader){.name_off = STRTAB_ISYM,
                           .type = SHT_SYMTAB,
                           .flags = SHF_INFO_LINK,
                           .info = 1,
                           .off = sizeof(ElfHeader) + core_sz + strtab_sz,
                           .virt_addr = 0,
                           .mem_sz = symtab_sz,
                           .align = 1,
                           .link = 1,
                           .ent_sz = sizeof(ElfSymtabEntry)};

    elf_contents =
        malloc(sizeof(ElfHeader) + core_sz + strtab_sz + symtab_sz + relas_sz);
    ARES_CHECK_OOM(elf_contents);
    size_t elf_off = 0;

    copy_n(elf_contents, &e_hdr, sizeof(e_hdr), &elf_off);
    copy_n(elf_contents, core, core_sz, &elf_off);
    copy_n(elf_contents, strtab, strtab_sz, &elf_off);
    copy_n(elf_contents, symtab, symtab_sz, &elf_off);
    copy_n(elf_contents, relas, relas_sz, &elf_off);

    *out = elf_contents;
    *len = elf_off;

    free(core);
    free(strtab);
    free(symtab);
    free(relas);
    return true;

fail:
    free(strtab);
    free(core);
    free(symtab);
    free(elf_contents);
    free(relas);
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

    if (0x7F != e_header->magic[0] || 'E' != e_header->magic[1] ||
        'L' != e_header->magic[2] || 'F' != e_header->magic[3]) {
        *error = "not an elf file";
        return false;
    }

    if (1 != e_header->bits) {
        *error = "unsupported elf variant (only elf32 is supported)";
        return false;
    }

    if (0xF3 != e_header->isa) {
        *error = "unsupported architecture (only risc-v is supported)";
        return false;
    }

    if (2 != e_header->type) {
        *error = "not an elf executable";
        return false;
    }

    ElfProgramHeader *phdrs =
        (ElfProgramHeader *)(elf_contents + e_header->phdrs_off);
    ElfSectionHeader *shdrs =
        (ElfSectionHeader *)(elf_contents + e_header->shdrs_off);

    ElfSectionHeader *str_tab_shdr = &shdrs[e_header->shdr_str_idx];
    char *str_tab = (char *)(elf_contents + str_tab_shdr->off);
    u32 str_tab_len = str_tab_shdr->mem_sz;

    for (u32 i = 0; i < e_header->shent_num; i++) {
        ElfSectionHeader *s_hdr = &shdrs[i];
        if (!(SHF_ALLOC & s_hdr->flags)) {
            continue;
        }

        Section *s = calloc(1, sizeof(Section));
        ARES_CHECK_OOM(s);
        s->read = true;
        s->align = s_hdr->align;
        s->base = s_hdr->virt_addr;
        s->contents.cap = s->contents.len = s_hdr->mem_sz;
        s->contents.buf = malloc(s->contents.len);
        ARES_CHECK_OOM(s->contents.buf);
        memcpy(s->contents.buf, elf_contents + s_hdr->off, s->contents.len);
        s->limit = s->base + s->contents.len;

        if (s_hdr->name_off >= str_tab_len) {
            *error = "section header name offset out of range";
            free(s);
            goto fail;
        }

        s->name = str_tab + s_hdr->name_off;

        if (SHF_WRITE & s_hdr->flags) {
            s->write = true;
        }

        if (SHF_EXECINSTR & s_hdr->flags) {
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

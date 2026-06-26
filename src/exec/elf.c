#include "ares/elf.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ares/core.h"
#include "ares/emulate.h"
#include "ares/util.h"

#define UNKNOWN_PROP "Unknown"

#define STRTAB_ISTR 1   // Index of .strtab in strtab
#define STRTAB_ISYM 9   // Index of .symtab in strtab
#define STRTAB_ISEC 17  // Start of section names in strtab

static bool sum_overflows(u32 a, u32 b) { return a > UINT32_MAX - b; }
static bool is_pow2(u32 val) { return val != 0 && (val & (val - 1)) == 0; }
static bool mul_overflows(u32 a, u32 b) {
    return a != 0 && b != 0 && a > UINT32_MAX / b;
}

static Elf32_Ehdr *ehdr_check(u8 *elf_contents, size_t elf_len,
                              char **out_error) {
    if (!elf_contents) {
        *out_error = "null buffer";
        return NULL;
    }

    if (elf_len < sizeof(Elf32_Ehdr)) {
        *out_error = "corrupt or invalid header";
        return NULL;
    }

    Elf32_Ehdr *ehdr = (void *)elf_contents;

    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        *out_error = "not an elf file";
        return NULL;
    }

    if (ehdr->e_ehsize != sizeof(Elf32_Ehdr)) {
        *out_error = "mismatched elf header size";
        return NULL;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) {
        *out_error = "not a 32 bit elf";
        return NULL;
    }

    return ehdr;
}

// pre: elf_contents points to a valid elf header
// pre: endianness compatibility guaranteed
static bool ehdr_check_offsets(Elf32_Ehdr *ehdr, size_t elf_len,
                               char **out_error) {
    if (ehdr->e_phentsize != sizeof(Elf32_Phdr) && ehdr->e_phnum > 0) {
        *out_error = "incompatible program header size";
        return false;
    }

    if (ehdr->e_shentsize != sizeof(Elf32_Shdr) && ehdr->e_shnum > 0) {
        *out_error = "incompatible section header size";
        return false;
    }

    if ((ehdr->e_phoff == 0 && ehdr->e_phnum > 0) || ehdr->e_phoff > elf_len ||
        mul_overflows(ehdr->e_phnum, sizeof(Elf32_Phdr)) ||
        sum_overflows(ehdr->e_phoff, ehdr->e_phnum * sizeof(Elf32_Phdr)) ||
        ehdr->e_phoff + ehdr->e_phnum * sizeof(Elf32_Phdr) > elf_len) {
        *out_error = "malformed program header information";
        return false;
    }

    if ((ehdr->e_shoff == 0 && ehdr->e_shnum > 0) || ehdr->e_shoff > elf_len ||
        mul_overflows(ehdr->e_shnum, sizeof(Elf32_Shdr)) ||
        sum_overflows(ehdr->e_shoff, ehdr->e_shnum * sizeof(Elf32_Shdr)) ||
        ehdr->e_shoff + ehdr->e_shnum * sizeof(Elf32_Shdr) > elf_len) {
        *out_error = "malformed section header information";
        return false;
    }

    if (ehdr->e_shstrndx >= ehdr->e_shnum && ehdr->e_shstrndx != SHN_UNDEF) {
        *out_error = "invalid section header string table index";
        return false;
    }

    return true;
}

// pre: ehdr is inside a larger buffer containing the entire ELF
static bool phdrs_check(Elf32_Ehdr *ehdr, size_t elf_len, char **out_error) {
    u8 *elf_contents = (void *)ehdr;
    Elf32_Phdr *phdrs = (void *)(elf_contents + ehdr->e_phoff);

    for (u32 i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr *phdr = phdrs + i;

        if (phdr->p_offset > elf_len ||
            sum_overflows(phdr->p_offset, phdr->p_filesz) ||
            phdr->p_offset + phdr->p_filesz > elf_len ||
            phdr->p_filesz > phdr->p_memsz ||
            sum_overflows(phdr->p_vaddr, phdr->p_memsz) ||
            phdr->p_flags & ~(PF_R | PF_W | PF_X) ||
            (phdr->p_align > 1 && !is_pow2(phdr->p_align)) ||
            (phdr->p_type == PT_LOAD && phdr->p_align > 1 &&
             phdr->p_vaddr % phdr->p_align != phdr->p_offset % phdr->p_align)) {
            *out_error = "one or more malformed program headers";
            return false;
        }
    }

    return true;
}

// pre: ehdr is inside a larger buffer containing the entire ELF
static bool shdrs_check(Elf32_Ehdr *ehdr, size_t elf_len, char **out_error) {
    u8 *elf_contents = (void *)ehdr;
    Elf32_Shdr *shdrs = (void *)(elf_contents + ehdr->e_shoff);
    Elf32_Shdr *str_sh = NULL;
    char *strtab = NULL;

    if (ehdr->e_shstrndx == SHN_UNDEF) goto check;
    str_sh = shdrs + ehdr->e_shstrndx;
    if (str_sh->sh_offset > elf_len ||
        sum_overflows(str_sh->sh_offset, str_sh->sh_size) ||
        str_sh->sh_offset + str_sh->sh_size > elf_len) {
        *out_error = "malformed section header string table";
        return false;
    }

    strtab = (void *)(elf_contents + str_sh->sh_offset);
    // We force the last byte to be \0 even if this is not standard. This way
    // C-string operations are safe, and worst case we're missing the last char
    // of a single string. This could cause issues in other contexts, but ARES
    // doesn't do symbol lookup anyway. Without this, there's no real guarantee
    // that ANY string in this buffer is terminated, so it would ALWAYS be
    // unsafe to touch without explicit bounds.
    // NOTE: this is altering state, but 1) the caller signed-in to this because
    // elf_read's contract is u8 *, not const u8 *; 2) the use case is bounded
    // to where this is safe.
    // NOTE: unfortunately copying is not really an alternative here!
    // NOTE: null terminating the whole string table **guarantees termination of
    // all substrings** as noted above
    if (str_sh->sh_size > 0) strtab[str_sh->sh_size - 1] = 0;

check:
    if (ehdr->e_shnum == 0) return true;
    if (shdrs[0].sh_type != SHT_NULL) {
        *out_error = "malformed null section";
        return false;
    }

    for (u32 i = 1; i < ehdr->e_shnum; i++) {
        Elf32_Shdr *shdr = shdrs + i;

        if ((str_sh != NULL && shdr->sh_name >= str_sh->sh_size) ||
            (shdr->sh_type != SHT_NOBITS &&
             (shdr->sh_offset > elf_len ||
              sum_overflows(shdr->sh_offset, shdr->sh_size) ||
              shdr->sh_offset + shdr->sh_size > elf_len)) ||
            shdr->sh_flags & ~(SHF_WRITE | SHF_ALLOC | SHF_EXECINSTR |
                               SHF_MERGE | SHF_STRINGS | SHF_INFO_LINK |
                               SHF_OS_NONCONFORMING | SHF_TLS) ||
            (shdr->sh_addralign > 1 && !is_pow2(shdr->sh_addralign)) ||
            (shdr->sh_entsize != 0 && shdr->sh_size % shdr->sh_entsize != 0)) {
            *out_error = "one or more malformed section headers";
            return false;
        }
    }

    return true;
}

// pre: ehdr is inside a larger buffer containing the entire ELF
static bool ehdr_check_entires(Elf32_Ehdr *ehdr, size_t elf_len,
                               char **out_error) {
    return ehdr_check_offsets(ehdr, elf_len, out_error) &&
           phdrs_check(ehdr, elf_len, out_error) &&
           shdrs_check(ehdr, elf_len, out_error);
}

static Elf32_Ehdr *elf_check_full(u8 *elf_contents, size_t elf_len,
                                  char **out_error) {
    Elf32_Ehdr *ehdr = ehdr_check(elf_contents, elf_len, out_error);
    if (!ehdr) return NULL;
    if (!ehdr_check_entires(ehdr, elf_len, out_error)) return NULL;

    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        *out_error = "not a little endian elf";
        return NULL;
    }

    if (ehdr->e_machine != EM_RISCV) {
        *out_error = "not a riscv elf";
        return NULL;
    }

    if (ehdr->e_ident[EI_OSABI] != ELFOSABI_SYSV) {
        *out_error = "not a sysv elf";
        return NULL;
    }

    return ehdr;
}

bool elf_read(u8 *elf_contents, size_t elf_len, ReadElfResult *out_res,
              char **out_error) {
    // Ensure pointers are NULL so free doesn't cause issues for uninitialized
    // inputs when this function fails
    out_res->phdrs = NULL;
    out_res->shdrs = NULL;

    if (!elf_contents) {
        *out_error = "null buffer";
        return false;
    }

    // NOTE: we check fewer things here because we want elf_read to work on a
    // wider range of files
    ReadElfSegment *readable_phdrs = NULL;
    ReadElfSection *readable_shdrs = NULL;
    Elf32_Ehdr *ehdr = ehdr_check(elf_contents, elf_len, out_error);
    if (!ehdr) return false;
    if (!ehdr_check_offsets(ehdr, elf_len, out_error)) return false;
    if (!shdrs_check(ehdr, elf_len, out_error)) return false;

    out_res->ehdr = ehdr;
    out_res->magic8 = elf_contents;
    out_res->clazz = "ELF32";

    // Print file endianness
    if (ehdr->e_ident[EI_DATA] == ELFDATA2LSB) {
        out_res->endianness = "Little endian";
    } else if (ehdr->e_ident[EI_DATA] == ELFDATA2MSB) {
        out_res->endianness = "Big endian";
    } else {
        out_res->endianness = UNKNOWN_PROP;
    }

    // Print OS/ABI
    if (ehdr->e_ident[EI_OSABI] == ELFOSABI_SYSV) {
        out_res->abi = "UNIX - System V";
    } else if (ehdr->e_ident[EI_OSABI] == ELFOSABI_LINUX) {
        out_res->abi = "Linux";
    }  // ...
    else {
        out_res->abi = UNKNOWN_PROP;
    }

    // Print ELF type
    if (ehdr->e_type == ET_REL) {
        out_res->type = "Relocatable";
    } else if (ehdr->e_type == ET_EXEC) {
        out_res->type = "Executable";
    } else if (ehdr->e_type == ET_DYN) {
        out_res->type = "Shared";
    } else if (ehdr->e_type == ET_CORE) {
        out_res->type = "Core";
    } else {
        out_res->type = UNKNOWN_PROP;
    }

    // Print architecture
    if (ehdr->e_machine == EM_RISCV) {
        out_res->architecture = "RISC-V";
    } else if (ehdr->e_machine == EM_ARM) {
        out_res->architecture = "ARM";
    } else if (ehdr->e_machine == EM_386) {
        out_res->architecture = "x86";
    } else {
        out_res->architecture = UNKNOWN_PROP;
    }

    Elf32_Phdr *phdrs = (void *)(elf_contents + ehdr->e_phoff);
    readable_phdrs = malloc(sizeof(ReadElfSegment) * ehdr->e_phnum);
    ARES_CHECK_OOM(readable_phdrs);

    for (u32 i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr *phdr = phdrs + i;
        ReadElfSegment *readable = readable_phdrs + i;
        size_t flags_idx = 0;

        readable->phdr = phdr;
        if (phdr->p_flags & PF_R) readable->flags[flags_idx++] = 'R';
        if (phdr->p_flags & PF_W) readable->flags[flags_idx++] = 'W';
        if (phdr->p_flags & PF_X) readable->flags[flags_idx++] = 'X';
        readable->flags[flags_idx] = 0;

        switch (phdr->p_type) {
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

    Elf32_Shdr *shdrs = (void *)(elf_contents + ehdr->e_shoff);
    readable_shdrs = malloc(sizeof(ReadElfSection) * ehdr->e_shnum);
    ARES_CHECK_OOM(readable_shdrs);

    // Guaranteed to be safe by shdrs_check
    Elf32_Shdr *str_sh = shdrs + ehdr->e_shstrndx;
    char *strtab = NULL;
    if (ehdr->e_shstrndx != SHN_UNDEF) {
        strtab = (void *)(elf_contents + str_sh->sh_offset);
    }

    // NOTE: we can access these without checks now, because we only get here if
    // checks passed earlier. THIS ALSO APPLIES TO sh_name!
    for (u32 i = 0; i < ehdr->e_shnum; i++) {
        Elf32_Shdr *shdr = shdrs + i;
        ReadElfSection *readable = readable_shdrs + i;
        size_t flags_idx = 0;

        readable->shdr = shdr;
        if (shdr->sh_flags & SHF_WRITE) readable->flags[flags_idx++] = 'W';
        if (shdr->sh_flags & SHF_ALLOC) readable->flags[flags_idx++] = 'A';
        if (shdr->sh_flags & SHF_STRINGS) readable->flags[flags_idx++] = 'S';
        if (shdr->sh_flags & SHF_EXECINSTR) readable->flags[flags_idx++] = 'X';
        readable->flags[flags_idx] = 0;
        if (strtab != NULL) readable->name = strtab + shdr->sh_name;
        else readable->name = UNKNOWN_PROP;

        switch (shdr->sh_type) {
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

    out_res->phdrs = readable_phdrs;
    out_res->shdrs = readable_shdrs;
    return true;
}

bool elf_load(u8 *elf_contents, size_t elf_len, char **error) {
    return false;
    /*
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
                memcpy(s->contents.buf, elf_contents + s_hdr->off,
    s->contents.len);

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
        */
}

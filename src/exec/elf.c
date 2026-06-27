#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "This code requires a little-endian target");
#else
#error "Cannot determine target endianness"
#endif

#include "ares/elf.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ares/core.h"
#include "ares/emulate.h"
#include "ares/util.h"

// SAFETY: an implicit precondition (maintained by callers in other files -
// axiom) for all functions whose arguments include something like u8
// *elf_contents is that the pointer be backed by malloc'd memory. The pointer
// itself shall either point to the start of the malloc'd buffer, or shall
// otherwise have equivalent alignment

// SAFETY: for functions that alter global or external state (e.g., elf_load),
// an additional precondition is that the caller not free u8 *elf_contents utill
// program exit, or untill global cleanup/reset

// NOTE: we do not support ELF files with entry (eh, ph, sh) sizes that don't
// match those of library structs. This violates the specification and rejects
// perfectly valid files, but it's rare, cursed, unlikely to be of any use here,
// and dangerous (easy to mess up)

#define UNKNOWN_PROP "Unknown"

static bool sum_overflows(u32 a, u32 b) { return a > UINT32_MAX - b; }
static bool is_pow2(u32 val) { return val != 0 && (val & (val - 1)) == 0; }
static bool mul_overflows(u32 a, u32 b) {
    return a != 0 && b != 0 && a > UINT32_MAX / b;
}

static Elf32_Ehdr *ehdr_check(u8 *elf_contents, u32 elf_len, char **out_error) {
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

    if (ehdr->e_ident[EI_VERSION] != EV_CURRENT ||
        ehdr->e_version != EV_CURRENT) {
        *out_error = "incompatible elf version";
        return NULL;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) {
        *out_error = "not a 32 bit elf";
        return NULL;
    }

    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        *out_error = "not a little endian elf";
        return NULL;
    }

    return ehdr;
}

// pre: elf_contents points to a valid elf header
// pre: endianness compatibility guaranteed
static bool ehdr_check_offsets(Elf32_Ehdr *ehdr, u32 elf_len,
                               char **out_error) {
    if (ehdr->e_phentsize != sizeof(Elf32_Phdr) && ehdr->e_phnum > 0) {
        *out_error = "incompatible program header size";
        return false;
    }

    if (ehdr->e_shentsize != sizeof(Elf32_Shdr) && ehdr->e_shnum > 0) {
        *out_error = "incompatible section header size";
        return false;
    }

    if ((ehdr->e_phoff == 0 && ehdr->e_phnum > 0) ||
        mul_overflows(ehdr->e_phnum, sizeof(Elf32_Phdr)) ||
        sum_overflows(ehdr->e_phoff, ehdr->e_phnum * sizeof(Elf32_Phdr)) ||
        ehdr->e_phoff + ehdr->e_phnum * sizeof(Elf32_Phdr) > elf_len ||
        ehdr->e_phoff % _Alignof(Elf32_Phdr) != 0) {
        *out_error = "malformed program header information";
        return false;
    }

    if ((ehdr->e_shoff == 0 && ehdr->e_shnum > 0) ||
        mul_overflows(ehdr->e_shnum, sizeof(Elf32_Shdr)) ||
        sum_overflows(ehdr->e_shoff, ehdr->e_shnum * sizeof(Elf32_Shdr)) ||
        ehdr->e_shoff + ehdr->e_shnum * sizeof(Elf32_Shdr) > elf_len ||
        ehdr->e_shoff % _Alignof(Elf32_Shdr) != 0) {
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
static bool phdrs_check(Elf32_Ehdr *ehdr, u32 elf_len, char **out_error) {
    u8 *elf_contents = (void *)ehdr;
    Elf32_Phdr *phdrs = (void *)(elf_contents + ehdr->e_phoff);

    for (u32 i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr *phdr = phdrs + i;

        // SAFETY: flags check against PF_R | PF_W | PF_X removed to allow files
        // with special flags (allowed by the specification). If used for
        // emulation (elf_load), unsupported flags will just be ignored
        if (sum_overflows(phdr->p_offset, phdr->p_filesz) ||
            phdr->p_offset + phdr->p_filesz > elf_len ||
            phdr->p_filesz > phdr->p_memsz ||
            sum_overflows(phdr->p_vaddr, phdr->p_memsz) ||
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
static bool shdrs_check(Elf32_Ehdr *ehdr, u32 elf_len, char **out_error) {
    u8 *elf_contents = (void *)ehdr;
    Elf32_Shdr *shdrs = (void *)(elf_contents + ehdr->e_shoff);
    Elf32_Shdr *str_sh = NULL;

    if (ehdr->e_shstrndx != SHN_UNDEF) {
        // SAFETY: bounds already checked in ehdr_check_offsets
        str_sh = shdrs + ehdr->e_shstrndx;
        if (str_sh->sh_type != SHT_STRTAB) {
            *out_error = "section header string table has wrong type";
            return false;
        }
    }

    if (ehdr->e_shnum == 0) return true;
    if (shdrs[0].sh_type != SHT_NULL) {
        *out_error = "malformed null section";
        return false;
    }

    // SAFETY: checking the null section is important, especially around the
    // name offset
    for (u32 i = 0; i < ehdr->e_shnum; i++) {
        Elf32_Shdr *shdr = shdrs + i;

        // SAFETY: as was done for phdrs, flag checks were removed to avoid
        // rejecting perfectly good files, espeicaly considering that the these
        // flags are never used
        if ((str_sh && shdr->sh_name >= str_sh->sh_size) ||
            (shdr->sh_type != SHT_NOBITS &&
             (sum_overflows(shdr->sh_offset, shdr->sh_size) ||
              shdr->sh_offset + shdr->sh_size > elf_len)) ||
            (shdr->sh_addralign > 1 && !is_pow2(shdr->sh_addralign)) ||
            (shdr->sh_entsize != 0 && shdr->sh_size % shdr->sh_entsize != 0) ||
            (shdr->sh_type == SHT_STRTAB && shdr->sh_size > 0 &&
             elf_contents[shdr->sh_offset + shdr->sh_size - 1] != 0)) {
            *out_error = "one or more malformed section headers";
            return false;
        }
    }

    return true;
}

// pre: ehdr is inside a larger buffer containing the entire ELF
static bool ehdr_check_entries(Elf32_Ehdr *ehdr, u32 elf_len,
                               char **out_error) {
    return ehdr_check_offsets(ehdr, elf_len, out_error) &&
           phdrs_check(ehdr, elf_len, out_error) &&
           shdrs_check(ehdr, elf_len, out_error);
}

static Elf32_Ehdr *elf_check_full(u8 *elf_contents, u32 elf_len,
                                  char **out_error) {
    Elf32_Ehdr *ehdr = ehdr_check(elf_contents, elf_len, out_error);
    if (!ehdr) return NULL;
    if (!ehdr_check_entries(ehdr, elf_len, out_error)) return NULL;

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

bool elf_read(u8 *elf_contents, u32 elf_len, ReadElfResult *out_res,
              char **out_error) {
    // SAFETY: ensure pointers are NULL so free doesn't cause issues for
    // uninitialized inputs when this function fails
    out_res->phdrs = NULL;
    out_res->shdrs = NULL;

    if (!elf_contents) {
        *out_error = "null buffer";
        return false;
    }

    // SAFETY: NOTE: we check fewer things here because we want elf_read to work
    // on a wider range of files
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
    } else {
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

    // SAFETY: bounds are already checked, but we want to avoid malloc(0)
    if (ehdr->e_phnum == 0) goto skip_phdrs;
    Elf32_Phdr *phdrs = (void *)(elf_contents + ehdr->e_phoff);
    readable_phdrs = malloc(sizeof(ReadElfSegment) * ehdr->e_phnum);
    ares_panic_if_null(readable_phdrs);

    for (u32 i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr *phdr = phdrs + i;
        ReadElfSegment *readable = readable_phdrs + i;
        u32 flags_idx = 0;

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

skip_phdrs:;
    // SAFETY: bounds are already checked, but we want to avoid malloc(0)
    if (ehdr->e_shnum == 0) goto skip_shdrs;
    Elf32_Shdr *shdrs = (void *)(elf_contents + ehdr->e_shoff);
    readable_shdrs = malloc(sizeof(ReadElfSection) * ehdr->e_shnum);
    ares_panic_if_null(readable_shdrs);

    // Guaranteed to be safe by shdrs_check
    Elf32_Shdr *str_sh = NULL;
    char *strtab = NULL;
    if (ehdr->e_shstrndx != SHN_UNDEF) {
        str_sh = shdrs + ehdr->e_shstrndx;
        strtab = (void *)(elf_contents + str_sh->sh_offset);
    }

    // SAFETY: we can access these without checks now, because we only get here
    // if checks passed earlier. THIS ALSO APPLIES TO sh_name! (checked in
    // shdrs_check)
    for (u32 i = 0; i < ehdr->e_shnum; i++) {
        Elf32_Shdr *shdr = shdrs + i;
        ReadElfSection *readable = readable_shdrs + i;
        u32 flags_idx = 0;

        readable->shdr = shdr;
        if (shdr->sh_flags & SHF_WRITE) readable->flags[flags_idx++] = 'W';
        if (shdr->sh_flags & SHF_ALLOC) readable->flags[flags_idx++] = 'A';
        if (shdr->sh_flags & SHF_STRINGS) readable->flags[flags_idx++] = 'S';
        if (shdr->sh_flags & SHF_EXECINSTR) readable->flags[flags_idx++] = 'X';
        readable->flags[flags_idx] = 0;
        if (strtab) readable->name = strtab + shdr->sh_name;
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

skip_shdrs:
    out_res->phdrs = readable_phdrs;
    out_res->shdrs = readable_shdrs;
    return true;
}

bool elf_load(u8 *elf_contents, u32 elf_len, char **out_error) {
    Elf32_Ehdr *ehdr = elf_check_full(elf_contents, elf_len, out_error);
    if (!ehdr) return false;

    if (ehdr->e_type != ET_EXEC) {
        *out_error = "not an elf executable";
        return false;
    }

    // Load program headers
    // TODO: check for overlapping segments
    Elf32_Phdr *phdrs = (void *)(elf_contents + ehdr->e_phoff);
    for (u32 i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr *phdr = phdrs + i;
        // SAFETY: zero-sized segments are problematic
        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0) continue;

        Section *s = calloc(1, sizeof(*s));
        ares_panic_if_null(s);
        s->read = phdr->p_flags & PF_R;
        s->write = phdr->p_flags & PF_W;
        s->execute = phdr->p_flags & PF_X;
        s->align = phdr->p_align;
        s->base = phdr->p_vaddr;
        s->name = UNKNOWN_PROP;

        s->contents.cap = s->contents.len = phdr->p_memsz;
        s->contents.buf = calloc(1, s->contents.len);
        ares_panic_if_null(s->contents.buf);
        // NOTE: we know file size <= memory size (checked above)
        memcpy(s->contents.buf, elf_contents + phdr->p_offset, phdr->p_filesz);
        s->limit = s->base + s->contents.len;

        *ARES_ARRAY_PUSH(&g_sections) = s;
    }

    // NOTE: no section header string table = no way to resolve names
    if (ehdr->e_shstrndx == SHN_UNDEF) goto exit;

    Elf32_Shdr *shdrs = (void *)(elf_contents + ehdr->e_shoff);
    Elf32_Shdr *str_sh = shdrs + ehdr->e_shstrndx;
    char *strtab = (void *)(elf_contents + str_sh->sh_offset);

    // Use a simple heuristic to resolve section names (when possible)
    // NOTE: section names are not crucial
    for (u32 i = 1; i < ehdr->e_shnum; i++) {
        Elf32_Shdr *shdr = shdrs + i;
        if (!(shdr->sh_flags & SHF_ALLOC)) continue;

        // Find ARES section with the same starting address as this ELF section
        Section *s = NULL;
        for (u32 j = 0; j < ARES_ARRAY_LEN(&g_sections) && !s; j++) {
            Section *c = *ARES_ARRAY_GET(&g_sections, j);
            if (c->base == shdr->sh_addr) s = c;
        }

        if (s) s->name = strtab + shdr->sh_name;
    }

exit:
    emulator_init();
    // NOTE: ARES does not generate native code, it runs instructions in an
    // emulation loop, and thus checks memory accesses when they happen. g_pc
    // can thus be assigned any value here
    g_pc = ehdr->e_entry;
    return true;
}

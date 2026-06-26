#ifndef _ELF_H
#define _ELF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"

typedef uint16_t Elf32_Half;
typedef uint16_t Elf64_Half;

typedef uint32_t Elf32_Word;
typedef int32_t Elf32_Sword;
typedef uint32_t Elf64_Word;
typedef int32_t Elf64_Sword;

typedef uint64_t Elf32_Xword;
typedef int64_t Elf32_Sxword;
typedef uint64_t Elf64_Xword;
typedef int64_t Elf64_Sxword;

typedef uint32_t Elf32_Addr;
typedef uint64_t Elf64_Addr;

typedef uint32_t Elf32_Off;
typedef uint64_t Elf64_Off;

typedef uint16_t Elf32_Section;
typedef uint16_t Elf64_Section;

typedef Elf32_Half Elf32_Versym;
typedef Elf64_Half Elf64_Versym;

#define EI_NIDENT (16)

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half e_type;
    Elf32_Half e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off e_phoff;
    Elf32_Off e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize;
    Elf32_Half e_phentsize;
    Elf32_Half e_phnum;
    Elf32_Half e_shentsize;
    Elf32_Half e_shnum;
    Elf32_Half e_shstrndx;
} Elf32_Ehdr;

#define EI_MAG0 0
#define ELFMAG0 0x7f

#define EI_MAG1 1
#define ELFMAG1 'E'

#define EI_MAG2 2
#define ELFMAG2 'L'

#define EI_MAG3 3
#define ELFMAG3 'F'

#define ELFMAG "\177ELF"
#define SELFMAG 4

#define EI_CLASS 4
#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2
#define ELFCLASSNUM 3

#define EI_DATA 5
#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2
#define ELFDATANUM 3

#define EI_VERSION 6

#define EI_OSABI 7
#define ELFOSABI_NONE 0
#define ELFOSABI_SYSV 0
#define ELFOSABI_HPUX 1
#define ELFOSABI_NETBSD 2
#define ELFOSABI_LINUX 3
#define ELFOSABI_GNU 3
#define ELFOSABI_SOLARIS 6
#define ELFOSABI_AIX 7
#define ELFOSABI_IRIX 8
#define ELFOSABI_FREEBSD 9
#define ELFOSABI_TRU64 10
#define ELFOSABI_MODESTO 11
#define ELFOSABI_OPENBSD 12
#define ELFOSABI_ARM 97
#define ELFOSABI_STANDALONE 255

#define EI_ABIVERSION 8

#define EI_PAD 9

#define ET_NONE 0
#define ET_REL 1
#define ET_EXEC 2
#define ET_DYN 3
#define ET_CORE 4

#define EM_NONE 0
#define EM_386 3
#define EM_ARM 40
#define EM_RISCV 243

#define EV_NONE 0
#define EV_CURRENT 1
#define EV_NUM 2

typedef struct {
    Elf32_Word sh_name;
    Elf32_Word sh_type;
    Elf32_Word sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off sh_offset;
    Elf32_Word sh_size;
    Elf32_Word sh_link;
    Elf32_Word sh_info;
    Elf32_Word sh_addralign;
    Elf32_Word sh_entsize;
} Elf32_Shdr;

#define SHN_UNDEF 0
#define SHN_LORESERVE 0xff00
#define SHN_LOPROC 0xff00
#define SHN_BEFORE 0xff00

#define SHN_AFTER 0xff01

#define SHN_HIPROC 0xff1f
#define SHN_LOOS 0xff20
#define SHN_HIOS 0xff3f
#define SHN_ABS 0xfff1
#define SHN_COMMON 0xfff2
#define SHN_XINDEX 0xffff
#define SHN_HIRESERVE 0xffff

#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_HASH 5
#define SHT_DYNAMIC 6
#define SHT_NOTE 7
#define SHT_NOBITS 8
#define SHT_REL 9
#define SHT_SHLIB 10
#define SHT_DYNSYM 11
#define SHT_INIT_ARRAY 14
#define SHT_FINI_ARRAY 15
#define SHT_PREINIT_ARRAY 16
#define SHT_GROUP 17
#define SHT_SYMTAB_SHNDX 18
#define SHT_RELR 19
#define SHT_NUM 20
#define SHT_LOOS 0x60000000
#define SHT_GNU_ATTRIBUTES 0x6ffffff5
#define SHT_GNU_HASH 0x6ffffff6
#define SHT_GNU_LIBLIST 0x6ffffff7
#define SHT_CHECKSUM 0x6ffffff8
#define SHT_LOSUNW 0x6ffffffa
#define SHT_SUNW_move 0x6ffffffa
#define SHT_SUNW_COMDAT 0x6ffffffb
#define SHT_SUNW_syminfo 0x6ffffffc
#define SHT_GNU_verdef 0x6ffffffd
#define SHT_GNU_verneed 0x6ffffffe
#define SHT_GNU_versym 0x6fffffff
#define SHT_HISUNW 0x6fffffff
#define SHT_HIOS 0x6fffffff
#define SHT_LOPROC 0x70000000
#define SHT_HIPROC 0x7fffffff
#define SHT_LOUSER 0x80000000
#define SHT_HIUSER 0x8fffffff

#define SHF_WRITE (1 << 0)
#define SHF_ALLOC (1 << 1)
#define SHF_EXECINSTR (1 << 2)
#define SHF_MERGE (1 << 4)
#define SHF_STRINGS (1 << 5)
#define SHF_INFO_LINK (1 << 6)
#define SHF_LINK_ORDER (1 << 7)
#define SHF_OS_NONCONFORMING (1 << 8)

#define SHF_GROUP (1 << 9)
#define SHF_TLS (1 << 10)
#define SHF_COMPRESSED (1 << 11)
#define SHF_MASKOS 0x0ff00000
#define SHF_MASKPROC 0xf0000000
#define SHF_ORDERED (1 << 30)
#define SHF_EXCLUDE (1U << 31)

typedef struct {
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Section st_shndx;
} Elf32_Sym;

typedef struct {
    Elf32_Half si_boundto;
    Elf32_Half si_flags;
} Elf32_Syminfo;

#define SYMINFO_BT_SELF 0xffff
#define SYMINFO_BT_PARENT 0xfffe
#define SYMINFO_BT_LOWRESERVE 0xff00

#define SYMINFO_FLG_DIRECT 0x0001
#define SYMINFO_FLG_PASSTHRU 0x0002
#define SYMINFO_FLG_COPY 0x0004
#define SYMINFO_FLG_LAZYLOAD 0x0008

#define SYMINFO_NONE 0
#define SYMINFO_CURRENT 1
#define SYMINFO_NUM 2

#define ELF32_ST_BIND(val) (((unsigned char)(val)) >> 4)
#define ELF32_ST_TYPE(val) ((val) & 0xf)
#define ELF32_ST_INFO(bind, type) (((bind) << 4) + ((type) & 0xf))

#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define STB_NUM 3
#define STB_LOOS 10
#define STB_GNU_UNIQUE 10
#define STB_HIOS 12
#define STB_LOPROC 13
#define STB_HIPROC 15

#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_SECTION 3
#define STT_FILE 4
#define STT_COMMON 5
#define STT_TLS 6
#define STT_NUM 7
#define STT_LOOS 10
#define STT_GNU_IFUNC 10
#define STT_HIOS 12
#define STT_LOPROC 13
#define STT_HIPROC 15

#define STN_UNDEF 0

#define ELF32_ST_VISIBILITY(o) ((o) & 0x03)

#define STV_DEFAULT 0
#define STV_INTERNAL 1
#define STV_HIDDEN 2
#define STV_PROTECTED 3

typedef struct {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
} Elf32_Rel;

typedef struct {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
    Elf32_Sword r_addend;
} Elf32_Rela;

typedef Elf32_Word Elf32_Relr;

#define ELF32_R_SYM(val) ((val) >> 8)
#define ELF32_R_TYPE(val) ((val) & 0xff)
#define ELF32_R_INFO(sym, type) (((sym) << 8) + ((type) & 0xff))

typedef struct {
    Elf32_Word p_type;
    Elf32_Off p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
} Elf32_Phdr;

#define PT_NULL 0
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_NOTE 4
#define PT_SHLIB 5
#define PT_PHDR 6
#define PT_TLS 7
#define PT_NUM 8
#define PT_LOOS 0x60000000
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_STACK 0x6474e551
#define PT_GNU_RELRO 0x6474e552
#define PT_GNU_PROPERTY 0x6474e553
#define PT_LOSUNW 0x6ffffffa
#define PT_SUNWBSS 0x6ffffffa
#define PT_SUNWSTACK 0x6ffffffb
#define PT_HISUNW 0x6fffffff
#define PT_HIOS 0x6fffffff
#define PT_LOPROC 0x70000000
#define PT_HIPROC 0x7fffffff

#define PN_XNUM 0xffff

#define PF_X (1 << 0)
#define PF_W (1 << 1)
#define PF_R (1 << 2)
#define PF_MASKOS 0x0ff00000
#define PF_MASKPROC 0xf0000000

#define R_RISCV_NONE 0
#define R_RISCV_32 1
#define R_RISCV_64 2
#define R_RISCV_RELATIVE 3
#define R_RISCV_COPY 4
#define R_RISCV_JUMP_SLOT 5
#define R_RISCV_TLS_DTPMOD32 6
#define R_RISCV_TLS_DTPMOD64 7
#define R_RISCV_TLS_DTPREL32 8
#define R_RISCV_TLS_DTPREL64 9
#define R_RISCV_TLS_TPREL32 10
#define R_RISCV_TLS_TPREL64 11
#define R_RISCV_TLSDESC 12

#define R_RISCV_BRANCH 16
#define R_RISCV_JAL 17
#define R_RISCV_CALL 18
#define R_RISCV_CALL_PLT 19
#define R_RISCV_GOT_HI20 20
#define R_RISCV_TLS_GOT_HI20 21
#define R_RISCV_TLS_GD_HI20 22
#define R_RISCV_PCREL_HI20 23
#define R_RISCV_PCREL_LO12_I 24
#define R_RISCV_PCREL_LO12_S 25
#define R_RISCV_HI20 26
#define R_RISCV_LO12_I 27
#define R_RISCV_LO12_S 28
#define R_RISCV_TPREL_HI20 29
#define R_RISCV_TPREL_LO12_I 30
#define R_RISCV_TPREL_LO12_S 31
#define R_RISCV_TPREL_ADD 32
#define R_RISCV_ADD8 33
#define R_RISCV_ADD16 34
#define R_RISCV_ADD32 35
#define R_RISCV_ADD64 36
#define R_RISCV_SUB8 37
#define R_RISCV_SUB16 38
#define R_RISCV_SUB32 39
#define R_RISCV_SUB64 40
#define R_RISCV_GOT32_PCREL 41
#define R_RISCV_ALIGN 43
#define R_RISCV_RVC_BRANCH 44
#define R_RISCV_RVC_JUMP 45
#define R_RISCV_RVC_LUI 46
#define R_RISCV_RELAX 51
#define R_RISCV_SUB6 52
#define R_RISCV_SET6 53
#define R_RISCV_SET8 54
#define R_RISCV_SET16 55
#define R_RISCV_SET32 56
#define R_RISCV_32_PCREL 57
#define R_RISCV_IRELATIVE 58
#define R_RISCV_PLT32 59
#define R_RISCV_SET_ULEB128 60
#define R_RISCV_SUB_ULEB128 61
#define R_RISCV_TLSDESC_HI20 62
#define R_RISCV_TLSDESC_LOAD_LO12 63
#define R_RISCV_TLSDESC_ADD_LO12 64
#define R_RISCV_TLSDESC_CALL 65

#ifdef __cplusplus
}
#endif

typedef struct {
    Elf32_Phdr *phdr;
    const char *type;
    char flags[4];
} ReadElfSegment;

typedef struct {
    Elf32_Shdr *shdr;
    const char *name;
    const char *type;
    char flags[5];
} ReadElfSection;

typedef struct {
    const char *name;
    const char *type;
    const char *bind;
    const char *visibility;
    const char *section;
    Elf32_Sym *entry;
} ReadElfSymtabEntry;

typedef struct {
    const char *name;
    Elf32_Shdr *shdr;
    ReadElfSymtabEntry *entries;
} ReadElfSymtab;

typedef struct {
    const char *type;
    const char *symbol;
    Elf32_Rel *entry;
} ReadElfRelaEntry;

typedef struct {
    const char *name;
    size_t num_entries;
    ReadElfRelaEntry *entries;
} ReadElfRelaSection;

typedef struct {
    Elf32_Ehdr *ehdr;
    const u8 *magic8;
    const char *clazz;
    const char *endianness;
    const char *abi;
    const char *type;
    const char *architecture;
    ReadElfSegment *phdrs;
    ReadElfSection *shdrs;
    size_t num_symtabs;
    ReadElfSymtab *symtab;
    size_t num_relas;
    ReadElfRelaSection *relas;
} ReadElfResult;

bool elf_read(u8 *elf_contents, size_t elf_contents_len, ReadElfResult *out,
              char **error);
bool elf_load(u8 *elf_contents, size_t elf_len, char **error);

#endif

/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_COMMON_ELF64_H
#define IRIS_COMMON_ELF64_H

#include <stdint.h>

/*
 * The ELF64 declarations this tree reads, in one place.
 *
 * They lived inside `svc_loader.c` while the loader was the only thing that
 * looked at an ELF.  `proc` is the second: it parses the header of the file it
 * read in order to answer `AT_PHDR`, `AT_PHENT`, `AT_PHNUM` and `AT_ENTRY` —
 * the auxiliary-vector entries a dynamic runtime cannot start without — and a
 * second private copy of these structures is a second thing to get wrong when
 * one of them changes.
 *
 * Only what is actually used is here.  An ELF header has more fields than this
 * system has reasons to read, and a declaration nobody reads is a declaration
 * nobody checks.
 */

typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

typedef struct {
    uint8_t    e_ident[16];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off  e_phoff;
    Elf64_Off  e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;

typedef struct {
    Elf64_Xword d_tag;
    Elf64_Xword d_val;
} Elf64_Dyn;

typedef struct {
    Elf64_Addr   r_offset;
    Elf64_Xword  r_info;
    Elf64_Sxword r_addend;
} Elf64_Rela;

#define ET_DYN              3u
#define EM_X86_64           62u
#define PT_LOAD             1u
#define PT_DYNAMIC          2u
/* PT_INTERP and PT_PHDR are read but not yet ACTED on: the loader rejects
 * nothing for having an interpreter, and `proc` reports one it cannot satisfy
 * as PROC_STEP_INTERP rather than loading it.  They are here because the
 * numbers are the ABI's and inventing them locally when step 4 arrives is how
 * two spellings of the same constant appear. */
#define PT_INTERP           3u
#define PT_PHDR             6u
#define DT_NULL             0
#define DT_RELA             7
#define DT_RELASZ           8
#define DT_RELAENT          9
#define PF_X                1u
#define PF_W                2u
#define ELF64_R_TYPE(i)     ((uint32_t)((i) & 0xffffffffULL))
#define R_X86_64_RELATIVE   8u

#endif /* IRIS_COMMON_ELF64_H */

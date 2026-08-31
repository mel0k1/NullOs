#ifndef ELF_H
#define ELF_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// ELF64 structures (minimal subset for loading)
// ============================================================

#define ELF_MAGIC    0x464C457F  // '\x7fELF'
#define ELF_CLASS64   2
#define ELF_DATA_LS   1           // Little-endian
#define ELF_TYPE_EXEC 2
#define ELF_MACHINE_X86_64  0x3E

// ELF header (64-bit)
typedef struct {
    u8  e_ident[16];     // Magic number and other info
    u16 e_type;          // Object file type
    u16 e_machine;       // Architecture
    u32 e_version;       // Object file version
    u64 e_entry;         // Entry point virtual address
    u64 e_phoff;         // Program header table file offset
    u64 e_shoff;         // Section header table file offset
    u32 e_flags;         // Processor-specific flags
    u16 e_ehsize;        // ELF header size
    u16 e_phentsize;     // Program header table entry size
    u16 e_phnum;         // Program header table entry count
    u16 e_shentsize;     // Section header table entry size
    u16 e_shnum;         // Section header table entry count
    u16 e_shstrndx;      // Section header string table index
} elf64_header_t;

// Program header entry (64-bit)
typedef struct {
    u32 p_type;          // Segment type
    u32 p_flags;         // Segment flags
    u64 p_offset;        // Segment file offset
    u64 p_vaddr;         // Segment virtual address
    u64 p_paddr;         // Segment physical address
    u64 p_filesz;        // Segment size in file
    u64 p_memsz;         // Segment size in memory
    u64 p_align;         // Segment alignment
} elf64_phdr_t;

// Segment types
#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_TLS     7

// Segment flags
#define PF_X 0x1           // Execute
#define PF_W 0x2           // Write
#define PF_R 0x4           // Read

// Default user-space load base (typical for x86_64 user programs)
#define ELF_USER_LOAD_BASE   0x400000ULL

// User stack size for loaded programs
#define ELF_USER_STACK_SIZE  (64 * 1024)  // 64 KB

// ============================================================
// ELF loader result
// ============================================================

#define ELF_MAX_SEGMENTS 16

typedef struct {
    bool valid;
    u64  entry_point;     // ELF entry point address
    u64  base_address;    // Lowest loaded address
    u64  top_address;     // Highest loaded address (base + memsz)
    u32  segment_count;   // Number of PT_LOAD segments loaded

    // Program header table (for AUXV: AT_PHDR/AT_PHENT/AT_PHNUM)
    u64  phoff;           // File offset of program header table
    u64  phnum;           // Number of program headers
    u64  phentsize;       // sizeof(Elf64_Phdr)

    struct {
        u64 vaddr;         // Virtual address
        u64 memsz;         // Memory size
        u64 filesz;        // File size
        u64 offset;        // File offset of this segment
        u32 flags;         // PF_R | PF_W | PF_X
    } segments[ELF_MAX_SEGMENTS];
} elf_load_info_t;

// ============================================================
// ELF loader API
// ============================================================

// Validate ELF64 header
// Returns true if the data looks like a valid ELF64 executable
bool elf_validate(const void* data, u64 size);

// Parse ELF and get load information (does NOT load into memory)
// Returns true on success, fills in info
bool elf_parse(const void* data, u64 size, elf_load_info_t* info);

// Load ELF segments into memory
// - data: pointer to ELF file content
// - size: size of ELF file
// - info: parsed info from elf_parse
// - load_base: if non-NULL, relocate all segments by this offset
// Returns true on success
bool elf_load(const void* data, u64 size, const elf_load_info_t* info, u64 load_base);

// Print ELF information
void elf_print_info(const elf_load_info_t* info);

// Load ELF from a file path (using fs_read)
// Returns true on success
bool elf_load_from_file(const char* path, elf_load_info_t* info);

// Execute a loaded ELF: maps user pages, builds the System V initial
// stack (argc/argv/envp/auxv — same layout as Linux), sets up the user
// stack, and iretq's to the entry point in ring 3.
//
// argc/argv are COPIED onto the new stack; argv[0] is conventionally
// the program path.
bool elf_execute(const void* data, u64 size, elf_load_info_t* info,
                 u64 load_base, int argc, char** argv);

// Shell command: elf <filepath>
//   "elf <path>" — parse only
//   "elf <path> run" — parse + load + execute in user mode
void cmd_elf(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif // ELF_H

#ifndef IRIS_ELF_LOADER_H
#define IRIS_ELF_LOADER_H

/*
 * ELF64 loader — loads a static ELF executable into an isolated address space.
 *
 * Usage:
 *   iris_elf_image_t img;
 *   iris_error_t err = elf_loader_load(data, size, &img);
 *   if (err != IRIS_OK) { ... }  // data is clean, nothing to free
 *   struct task *t = task_spawn_elf(&img, arg0);
 *   if (!t) { elf_loader_free_image(&img); }
 *
 * Constraints:
 *   - Only ET_EXEC (static) ELFs are supported; ET_DYN requires a dynamic linker.
 *   - Only EM_X86_64 is accepted.
 *   - All PT_LOAD segments must fall within the user private window
 *     (USER_PRIVATE_BASE .. USER_PRIVATE_BASE + USER_PRIVATE_SIZE).
 *   - At most ELF_LOADER_MAX_LOAD_SEGS PT_LOAD segments are loaded.
 *   - The entry point must lie within one of the loaded segments.
 */

#include <stdint.h>
#include <iris/nc/error.h>

/* Maximum number of PT_LOAD segments we support per ELF image. */
#define ELF_LOADER_MAX_LOAD_SEGS  8u

/*
 * iris_elf_image_t — result of a successful elf_loader_load call.
 *
 * This struct tracks every physical resource allocated by the loader so
 * that task_spawn_elf (on success) or elf_loader_free_image (on failure)
 * can take ownership or release them cleanly.
 *
 * Fields:
 *   entry_vaddr  — ELF e_entry; virtual address of the process entry point.
 *   cr3_phys     — physical address of the new PML4 page table root.
 *   seg_count    — number of PT_LOAD segments loaded (≤ ELF_LOADER_MAX_LOAD_SEGS).
 *   segs[]       — physical base and page count for each loaded segment;
 *                  used by KProcess for cleanup on process exit.
 */
typedef struct {
    uint64_t  entry_vaddr;
    uint64_t  cr3_phys;
    uint32_t  seg_count;
    struct {
        uint64_t phys_base;   /* physical base of allocated pages for this segment */
        uint32_t page_count;  /* number of 4 KiB pages */
    } segs[ELF_LOADER_MAX_LOAD_SEGS];
} iris_elf_image_t;

/*
 * elf_loader_load — parse and load an ELF64 executable into a new address space.
 *
 * @data   Kernel virtual pointer to the ELF image (e.g. from the initrd).
 * @size   Byte length of the ELF image; used for bounds checking.
 * @out    Filled with entry point, CR3, and segment info on success.
 *
 * On success returns IRIS_OK and *out is populated.
 * On any failure returns a negative iris_error_t and no resources are leaked
 * (all allocated pages and page tables are freed before returning).
 *
 * Does NOT allocate or map the user stack; that is task_spawn_elf's job.
 */
iris_error_t elf_loader_load(const void *data, uint32_t size,
                             iris_elf_image_t *out);

/*
 * elf_loader_free_image — release all resources from a failed spawn attempt.
 *
 * Call this ONLY on the error path after elf_loader_load succeeded but
 * task_spawn_elf (or subsequent bootstrap setup) failed.  On the success
 * path the task and KProcess own the resources and must NOT call this.
 *
 * Safe to call with a zeroed iris_elf_image_t (no-op).
 */
void elf_loader_free_image(iris_elf_image_t *img);

#endif /* IRIS_ELF_LOADER_H */

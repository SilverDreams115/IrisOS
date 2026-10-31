# The persistent store, version 2 — a format that does not need replacing

**Status: DESIGN. Nothing in this document is implemented.** It is written
before the code so that the decisions can be argued with while they are still
cheap, and so that the one that is usually skipped — crash consistency — is
decided first rather than discovered last.

## What version 1 is, and why it ends

`services/fs/main.c`, 442 lines, over a 140-line protocol:

    sector 0        superblock
    sector 1        directory: 16 entries of 32 bytes
    sector 16..     one sector per file, in directory order

    FS_MAX_FILES  16      FS_SECTOR 512      FS_NAME_BYTES 16

Sixteen files of five hundred and twelve bytes. Eight kilobytes of storage in
total, and a file that cannot grow. The boot report already does not fit:
542 bytes written into a 512-byte file, truncated, and `init` says so
(`report written to boot.rep (CUT to one sector)`).

**It ends because it is the gate on everything above it.** A shell with
history, a configuration anybody can edit, a font, a credential store, a log
that survives a boot — each is a file that grows, and none of them is possible
at 512 bytes. This is not a performance problem and no amount of tuning
addresses it.

## What version 1 got right, and is kept

Three things, and they are the expensive ones:

- **Formatting is an AUTHORITY, not an assumption.** A disk with neither an
  IRIS superblock nor the `S-FMT-OK` token at offset 496 is refused: not
  formatted, not mounted, reported as foreign, left as found. That reasoning
  is in `fs_ep_proto.h` and it stays verbatim. Version 2 widens a format; it
  does not widen who may destroy a disk.
- **The disk is ASKED FOR.** `fs` asks `blk` which disk carries an IRIS
  partition (`BLK_OP_HOME`) rather than trusting an index that is correct on
  one machine.
- **Persistence is MEASURED.** The generation counter is read, incremented and
  written back at mount, so a second boot over the same image reports a higher
  number. That is what makes "persistent" a verdict instead of a plan, and
  version 2 keeps the counter for exactly that reason.

## The constraint that fixes the block size

`blk` moves **`BLK_MAX_SECTORS` = 8 sectors = 4 KiB** per call, which is one
frame, which is the unit the buffer-capability handshake (`FS_OP_BUF`) already
hands a client.

So **the block is 4 KiB**: one block is one transfer is one frame. Any other
choice buys a partial-frame path in three places and buys nothing back.

## Scale, stated as numbers rather than as "scalable"

| quantity | width | ceiling | reached when |
|---|---|---|---|
| block number | 32-bit | 2^32 x 4 KiB = **16 TiB** | a disk larger than any this project will see |
| file size | 64-bit | — | never |
| file count | directory is itself a file | **unbounded** | never |
| name length | 64 bytes | — | a path component longer than 64 bytes |

The 1 GiB partition on the development machine is **262144 blocks**. A 32-bit
block number is therefore four orders of magnitude of headroom, and the
remaining fixed widths are chosen so that the format is not the thing that
runs out.

## The decision that matters: crash consistency, by construction

Version 1 is crash-safe **by accident of being trivial**. A file is one
sector, so a write is a single atomic sector write: power can fail before it
or after it, never inside it. The moment a file spans blocks that property is
gone, and the usual answer is a journal — a write-ahead log, plus replay, plus
the log's own failure modes.

**Version 2 takes the other road: copy-on-write with an atomic superblock
flip.** Nothing is ever overwritten in place. A write allocates new blocks,
builds the new metadata in new blocks, and then commits by writing **one
sector**: a superblock naming the new root.

That reduces the entire crash story to the assumption version 1 already
depends on and the hardware already gives — *a single sector write either
happens or does not* — and it keeps the property by DESIGN rather than by
being too small to break.

Two superblocks, A at sector 0 and B at sector 1, each carrying a generation
and a checksum. Mount reads both and takes the valid one with the higher
generation. Commit always writes the older of the two. A power cut during a
commit leaves the other superblock intact and the previous tree reachable: the
mount is **old-or-new, never a mixture**, and that sentence is the whole
specification.

### And it is a LANE, not a claim

The failure mode of every crash-consistency design is that it works in every
test that does not cut the power. So the design is not finished until there is
a lane that:

1. starts a multi-block write,
2. kills QEMU at a chosen point inside it,
3. mounts the image again,
4. asserts the content is **exactly** the old file or **exactly** the new one,
5. and runs that for a spread of kill points, including during the commit
   sector itself.

Without it, "crash-safe" is an intention. The generation counter is the
precedent: it turned "persistent" into something a boot either reports or does
not.

## Layout

    block 0      superblock A        (one sector used, rest reserved)
    block 1      superblock B
    block 2..    allocation bitmap   (ceil(nblocks / 8 / 4096) blocks)
    ...          inode table         (grows as a file, see below)
    ...          data

Both superblocks name the root directory's inode and the bitmap's extent, so
a commit that moves either is still one sector.

### Inodes, and why extents

    struct fs_inode {            /* 64 bytes */
        uint64_t size;           /* bytes */
        uint64_t mtime_ns;
        uint32_t flags;          /* type: file | directory */
        uint32_t nlink;
        struct { uint32_t start, count; } extent[4];   /* inline */
        uint32_t overflow;       /* block of further extents, 0 = none */
        uint32_t reserved;
    };

Four inline extents reach 4 contiguous runs with no extra read, which covers
every file this system writes today and most it will write tomorrow. A file
that fragments past four runs spends one more block and one more read. The
alternative — indirect blocks, ext2-style — costs three levels of lookup code
for files that do not exist here.

**Extents are also what makes copy-on-write cheap**: rewriting a file's tail
allocates a new run and rewrites one inode, not a chain.

### The directory is a file

A directory is an inode whose content is a sequence of

    struct fs_dirent {           /* 128 bytes */
        uint8_t  name[64];
        uint32_t inode;
        uint32_t flags;
        uint8_t  reserved[56];
    };

Thirty-two per 4 KiB block, and the directory grows by the same mechanism as
any other file. That is what retires `FS_MAX_FILES` without putting a new
constant in its place.

Nested directories fall out of this for free, and version 2 should NOT use
them at first: a flat root is what the callers need, and a path walk is
separable work with its own tests.

## The protocol, and the eighteen callers

Eighteen files call into `fs` today — `vfs`, `sh`, `proc`, `pager`, `objreg`,
`libc/fd.c` and six test files among them. The protocol they use is four
operations: `FS_OP_BUF`, `FS_OP_READ`, `FS_OP_STAT`, `FS_OP_WRITE`, and a name
arrives packed into `words[0..1]` with the length in `words[2]`.

**There is no offset.** Files are read and written whole. `libc`'s `lseek`
works because the VFS holds the offset in memory and re-reads.

So the protocol gains exactly one thing: an **offset** on `FS_OP_READ` and
`FS_OP_WRITE`. The VFS already tracks per-descriptor offsets, so it is the
natural place to pass them down, and `lseek` stops being a fiction over whole
files.

`VFS_EP_OP_FILE_WRITE` does not exist. Adding it is the point at which a
program can keep something, and it is deliberately listed as a SEPARATE step
below: a format that cannot be written through is not useful, but a format
that is wrong is worse, and the order matters.

## What this does NOT cost, and why that is the architecture paying out

- **Zero kernel changes.** `fs` is ring-3. This touches none of the 37 charter
  invariants, none of the authority model, and no syscall.
- **It does not count against the `check-loops` ceiling.** That ceiling is
  12000 code lines over "core" — the files `spec_driver.c` links — currently
  **4904**. `services/fs` is not among them. So the store can be as large as
  correctness requires without spending the budget a refinement proof would
  have to cover.
- **No new driver and no new DMA path.** `blk` already does AHCI over real
  hardware, and its DMA is already contained by an IOSpace.
- **No new authority.** `fs` receives its disk and its budget exactly as now.

A filesystem that cannot break the kernel's guarantees, by construction, is
what putting it outside the kernel was for.

## Migration: refuse, do not convert

A version-2 `fs` meeting a version-1 disk **refuses and says so**, and the
disk is re-made only by whoever holds the format authority.

Automatic conversion on first mount is rejected for the reason the version-1
header already gives about automatic formatting: the machine that is right
about owning a disk is the one that was told it owns it. A conversion that
runs unasked is the same mistake with a friendlier name.

The development disk is 1 GiB and holds a report. Losing it costs nothing; a
disk that mattered would cost everything, and the format authority is the
line.

## Order of work, and what closes each step

1. **Format + allocator + mount, read-only.** Closes when a version-2 image
   made by a host tool mounts, lists and reads back files larger than one
   block, and a version-1 image is refused with its own message.
2. **Copy-on-write write path + the commit flip.** Closes when the crash lane
   above passes over a spread of kill points.
3. **The offset in the protocol.** Closes when `lseek` + `read` over a
   multi-block file returns the same bytes as reading it whole, and the
   eighteen callers are green.
4. **`VFS_EP_OP_FILE_WRITE`.** Closes when a C program writes a file, exits,
   and a later boot reads it back — the generation counter's trick applied to
   content instead of to mount count.
5. **Directories, if wanted.** Separable, and not required by anything above.

Steps 1 and 2 are the format and are not separable from each other: a store
that can be written without a commit discipline would have to be rewritten.
Steps 3 onward are additive.

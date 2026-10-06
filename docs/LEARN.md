# Learn

Short explanations of the ideas in this project. Simple words, one real example each. New entries are added as we meet new ideas.

## Deduplication
Store each piece of data only once. If two backups contain the same 16 KiB piece, the repo keeps one copy and
both backups point to it. Example: the SeqCDC paper measured about 50% savings on Linux kernel releases,
so our 17 GB of kernel trees (D1) should need roughly 9 GB in the repo.

## Chunk
A piece of a file. We cut files into chunks of about 16 KiB on average. Each chunk has a minimum size
(¼ to ½ of the average, so 4–8 KiB) and a maximum (2× the average, 32 KiB). A 1 GiB file becomes about 65 000 chunks.

## Content-defined chunking (CDC)
Choosing where to cut by looking at the bytes, not by counting. Why it matters: if you insert 1 byte at the
start of a file and cut every 16 KiB exactly, *every* chunk shifts and nothing dedups. With CDC, the cut points
follow the content, so only the first chunk changes and the other 65 000 still match.

## SeqCDC
A CDC method from 2024. It cuts after 5 bytes in a row that keep going up (example: 12, 40, 41, 90, 200).
It needs no hash, so it is very fast. The paper measured about 30 GB/s on one core with AVX-512.

## SIMD, AVX2, AVX-512
One CPU instruction that works on many bytes at once. AVX2 handles 32 bytes per instruction, AVX-512 handles 64.
Example: to check whether byte i+1 > byte i for 64 positions, a normal loop needs 64 comparisons; AVX-512 needs 1.
Your Ryzen 7 8845HS has both.

## Hash (BLAKE3)
A 32-byte "fingerprint" of a chunk. Same chunk → same fingerprint; different chunk → different fingerprint
(a collision is so unlikely we ignore it). We compare fingerprints instead of whole chunks.
BLAKE3 hashes about 3+ GB/s per core on this CPU.

## Compression (zstd)
Makes data smaller. Text often shrinks to about 1/3; random data does not shrink at all, so we store it as is.

## Cold cache vs warm cache
Linux keeps recently read files in RAM (the "page cache"). A *warm* run reads from RAM and is fast; a *cold* run
must read from the disk. Example: a 4 GiB file can be read from RAM in about 0.5 s, but from this NVMe it takes
longer. Fair benchmarks say which one they measured.

## WSL2 virtual disk
Linux on Windows (WSL2) does not see your NVMe directly. It sees one big file on C: (`ext4.vhdx`) that acts as a
disk. It works well, but it adds a layer, so disk speeds measured in Linux are lower than the drive's rated 7 GB/s.

## Memory bandwidth
How many bytes per second the CPU can read from RAM. On this laptop we measured about **23 GB/s**, and it is
reached already with 2 threads: more threads do not help. That is low for this CPU because the laptop has one
memory module (one "channel"); two modules would give roughly twice as much. Example: chunking a 4 GiB buffer
that is in RAM can never go faster than 4.3 GB ÷ 23 GB/s ≈ 0.19 s, however fast our code is.

## L2 cache
A small, very fast memory inside each CPU core: 1 MiB per core here. Data that fits in L2 can be read many times
faster than from RAM. That is why we chunk and hash each 1 MiB block in one go (D5): the bytes come from RAM once.

## O_DIRECT
A way to read or write a file that skips Linux's file cache and talks to the disk directly. Disk-speed tests use
it so we measure the disk, not RAM. Measured here: reads about **6 GB/s**, writes about **1.5–1.7 GB/s**.

## Page cache copy
A normal `read()` of a file that is already cached copies the bytes from Linux's cache into our program's buffer.
That copy costs RAM bandwidth too. Measured here: 8 readers copy 9–13 GB/s out of the cache, before doing any work.
So a "warm" backup cannot go faster than that unless it avoids the copy (for example with `mmap`).

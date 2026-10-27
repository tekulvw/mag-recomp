# Native graphics reimplementation method

Native replacements for the retail SPU render jobs (MaterialFlush Main, DepthNormal, Shadow, Other and later stages) are independent reimplementations. They are not ports, transliterations or re-expressions of the original instruction streams.

## Rules

1. **Two steps: analysis, then implementation.** The analysis step may read the original code (disassembly, SDK-lifted C, decompiler output) and development-build debug information to understand behavior and data structures. Its product is a written behavioral specification in our own words. The implementation step works from that specification.
2. **Nothing copied from original code enters Git.** Disassembly, decompiler output, instruction or function hashes, constant tables, embedded strings and byte dumps stay in private scratch space. Documents describe behavior in original wording; guest data-structure layouts are stated only as the interface facts needed to read and write guest memory.
3. **Specification content.** For each job, write a behavioral specification: guest-visible inputs (job header, descriptors, source buffers), guest-visible outputs (command words, tables, markers), ordering and completion rules, and error/edge cases. Public hardware formats (RSX/GCM method layout) are cited as interface facts. The specification states *what*, never the original's step order, register use or control flow.
4. **Implement from the specification.** The implementation chooses its own algorithms, data layout, batching and parallelism. Structural similarity to the original is a defect to remove, not a goal.
5. **The original is a test oracle.** Differential tests run the original module in the SDK interpreter on privately held bytes and compare observable results (output memory, commands, completion state) against the native result. Tests contain generators and comparisons, never the original's code.
6. **Retail is the authority.** Behavior is confirmed against the retail v1.0 build. The development build helps with naming and orientation only and its addresses are not applied to retail.
7. **Efficiency is a design input.** Because outputs, not steps, must match, native code may fuse, reorder internally, vectorize or parallelize as long as guest-visible results and completion order are preserved.

## Status

This repository contains no native render-job replacements: every SPU job runs as the recompiled original program. New native code must be developed under this method from the start.

Black-box capture and analysis tools: `tools/jobio.py` (recording reader), `tools/jobio_provenance.py` (output-to-input relations), `tools/jobio_rules.py` (item-level rule mining with held-out scoring).

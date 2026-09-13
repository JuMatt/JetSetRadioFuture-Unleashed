# The JSRF-specific half

Everything in this repository except this folder and `launcher/` is
title-agnostic machinery — the kernel layer, the NV2A and MCPX models, the
recompilation pipeline. These two files are the part that is about *Jet Set
Radio Future* and nothing else.

- **`main.c`** — the port's entry point. Loads the XBE for its data sections,
  maps the Xbox address space, starts the kernel and the hardware models, and
  calls the recompiled entry point. It knows the game's entry-point address
  and where to find its files.

- **`recomp_manual.c`** — hand-written overrides for individual recompiled
  functions, by guest address. Some are fixes for functions the lifter got
  wrong; most are instrumentation that answers a specific question about a
  specific fault, kept because the question tends to come back. The comments
  say which fault each one was written for.

The bulk of the port — the hundreds of thousands of lines of C the pipeline
generates from the XBE — is **not** here and will not be: it is derived from a
copyrighted binary. It is produced on your machine, from your own disc, by the
pipeline in `tools/`.

#!/usr/bin/env python3
"""Build-only exact SDK drain extraction for the native bridge regression."""
import argparse
from pathlib import Path


def extract(source: Path, output: Path) -> None:
    text = source.read_text()
    begin = text.index("typedef struct spu_irq_frame {")
    end = text.index("} spu_irq_frame;", begin) + len("} spu_irq_frame;")
    function = text.index("typedef void (*spu_drain_fn)(spu_context*);")
    # SDK drain is deliberately copied verbatim into the build directory. The
    # test stubs external scheduling/IRQ hooks, not the drain implementation.
    output.write_text('#include "spu_context.h"\n#include <setjmp.h>\n' +
                      text[begin:end] + "\n" + text[function:])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    extract(args.source, args.output)

#!/usr/bin/env python3
"""Maintainer-only local source kit entry point; never publishes."""
from pathlib import Path
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from mag_installer.native_kit import main

if __name__ == '__main__':
    raise SystemExit(main())

"""PyInstaller entry point; no game assets or external Python are required."""
from mag_installer.__main__ import main

if __name__ == '__main__':
    raise SystemExit(main())

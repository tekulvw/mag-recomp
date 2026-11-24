"""How many compile jobs a player build runs at once, and the player's remembered choice.

The launcher and engine compile runs ``cmake --build --parallel N`` (and sets
``CMAKE_BUILD_PARALLEL_LEVEL``) with N from here. Installers up to 0.8.0 always used 2, because the
generated translation units are large: a single PPU unit takes about 3 to 3.5 GB of memory to
compile, and too many at once exhaust memory (on a Linux machine without swap the kernel then kills
processes). So the default is set by memory, and capped by the computer's logical CPUs:

    default = min(logical CPUs, (total memory - MEMORY_RESERVE) // JOB_MEMORY), at least 2

``JOB_MEMORY`` (4 GiB) is the largest measured peak of one compile with headroom, so every job may
compile one of the largest units at the same time. ``MEMORY_RESERVE`` (4 GiB) is left for the
operating system, the installer and the rest of the desktop. The floor of 2 is the count every
earlier installer used, which builds on the smallest supported computers. If memory or CPUs cannot
be read, the default is 2.

A player may choose any whole number from 1 to 64 (the wizard's Compile jobs field, or
``build --jobs N``). The choice is remembered in the installer's per-user cache folder
(``preferences.json`` beside the downloads and the build cache; never in a MAG folder) and used by
later builds until the player chooses again; ``auto`` forgets it and returns to the computer's
default. The job count only changes how many compiles run at once: it is not part of any build
cache key or retry check, so changing it reuses earlier work and a Retry build may use another one.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import sys

from .model import InstallerError

MIN_JOBS = 1
MAX_JOBS = 64
FALLBACK_JOBS = 2
GIB = 1024 ** 3
JOB_MEMORY = 4 * GIB
MEMORY_RESERVE = 4 * GIB
PREFERENCES = 'preferences.json'
SCHEMA = 1
AUTO = 'auto'


def logical_cpus():
    """The logical CPUs this process may use, or None when unknown."""
    if hasattr(os, 'sched_getaffinity'):
        try:
            return len(os.sched_getaffinity(0)) or None
        except OSError:
            pass
    counter = getattr(os, 'process_cpu_count', None) or os.cpu_count
    return counter() or None


def total_memory():
    """Installed physical memory in bytes, or None when unknown."""
    if sys.platform == 'win32':
        try:
            import ctypes
            from ctypes import wintypes

            class MemoryStatus(ctypes.Structure):
                _fields_ = [('dwLength', wintypes.DWORD), ('dwMemoryLoad', wintypes.DWORD),
                            ('ullTotalPhys', ctypes.c_ulonglong), ('ullAvailPhys', ctypes.c_ulonglong),
                            ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                            ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong),
                            ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]
            status = MemoryStatus()
            status.dwLength = ctypes.sizeof(MemoryStatus)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
                return int(status.ullTotalPhys) or None
        except (OSError, AttributeError, ValueError):
            pass
        return None
    try:
        pages, size = os.sysconf('SC_PHYS_PAGES'), os.sysconf('SC_PAGE_SIZE')
        if pages > 0 and size > 0:
            return pages * size
    except (OSError, ValueError, AttributeError):
        pass
    return None


def recommended(cpus=None, memory=None):
    """The default job count for a computer with these logical CPUs and bytes of memory."""
    cpus = logical_cpus() if cpus is None else cpus
    memory = total_memory() if memory is None else memory
    if not cpus or not memory:
        return FALLBACK_JOBS
    by_memory = max(0, memory - MEMORY_RESERVE) // JOB_MEMORY
    return max(min(FALLBACK_JOBS, cpus), min(cpus, by_memory, MAX_JOBS))


def parse(value):
    """A job count from the player (an int or its text); a clear error for anything else."""
    text = str(value).strip()
    if type(value) is bool or not text.isascii() or not text.isdigit() or not MIN_JOBS <= int(text) <= MAX_JOBS:
        raise InstallerError(f'Compile jobs must be a whole number from {MIN_JOBS} to {MAX_JOBS} '
                             f'(or {AUTO} for this computer\'s default, {recommended()}); got {value!r}.')
    return int(text)


def _path(cache):
    return Path(cache).expanduser().absolute() / PREFERENCES


def _read(cache):
    path = _path(cache)
    try:
        if path.is_symlink() or not path.is_file():
            return {}
        data = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) and data.get('schema_version') == SCHEMA else {}


def remembered(cache):
    """The job count the player chose for an earlier build, or None."""
    value = _read(cache).get('jobs')
    return value if type(value) is int and MIN_JOBS <= value <= MAX_JOBS else None


def remember(cache, jobs):
    """Keep (or, with None, forget) the player's job count for later builds."""
    path = _path(cache)
    if path.parent.is_symlink():
        raise InstallerError(f'The installer cache folder must not be a symlink: {path.parent}')
    data = {key: value for key, value in _read(cache).items() if key != 'jobs'}
    data['schema_version'] = SCHEMA
    if jobs is not None:
        data['jobs'] = jobs
    path.parent.mkdir(parents=True, exist_ok=True)
    from .pipeline import _write_json
    _write_json(path, data)


def resolve(cache, requested=None):
    """The job count for a build and where it came from ('chosen', 'remembered' or 'default').

    requested: None uses the remembered choice, else the default; ``auto`` forgets the remembered
    choice and uses the default; a number is checked, remembered and used. Nothing is written for
    an invalid value."""
    if requested is None:
        value = remembered(cache)
        return (value, 'remembered') if value is not None else (recommended(), 'default')
    if str(requested).strip().lower() == AUTO:
        remember(cache, None)
        return recommended(), 'default'
    value = parse(requested)
    remember(cache, value)
    return value, 'chosen'


def describe(jobs, source):
    """One line for the build log."""
    cpus, memory = logical_cpus(), total_memory()
    computer = (f'{cpus} logical CPUs, {memory / GIB:.0f} GiB memory' if cpus and memory else 'CPUs or memory unknown')
    if source == 'default':
        why = f'this computer\'s default, for {computer}'
    else:
        why = (('your choice, remembered for later builds' if source == 'chosen' else 'your earlier choice')
               + f'; this computer\'s default is {recommended(cpus, memory)}')
    return (f'Compiling with {jobs} job{"s" if jobs != 1 else ""} at once ({why}). '
            'Change it with the Compile jobs field or build --jobs N.')

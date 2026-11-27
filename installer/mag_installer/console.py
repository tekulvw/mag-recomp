"""Text output that survives whatever encoding the console or a redirected file uses.

On Windows, output redirected to a file or pipe is encoded with the ANSI code page
(cp1252 on most Western systems), which has no arrows, dashes beyond its own set, or
most non-Latin text. A frozen installer also ignores PYTHONIOENCODING and PYTHONUTF8,
so the player cannot fix it from outside. A windowed program may have no streams.
"""
import io
import sys


def use_utf8_streams():
    """Switch stdout and stderr to UTF-8, replacing anything still unencodable.

    Streams that are missing (None) or cannot be reconfigured are left as they are;
    write_line still keeps them from failing on text they cannot encode."""
    for name in ('stdout', 'stderr'):
        stream = getattr(sys, name, None)
        reconfigure = getattr(stream, 'reconfigure', None)
        if reconfigure is None:
            continue
        try:
            reconfigure(encoding='utf-8', errors='replace')
        except (ValueError, OSError, LookupError, io.UnsupportedOperation):
            pass


def write_line(stream, text):
    """Write one line and flush; characters the stream cannot encode become replacements."""
    if stream is None:
        return
    line = f'{text}\n'
    try:
        stream.write(line)
    except UnicodeEncodeError:
        encoding = getattr(stream, 'encoding', None) or 'ascii'
        try:
            safe = line.encode(encoding, 'replace').decode(encoding, 'replace')
        except LookupError:
            safe = line.encode('ascii', 'replace').decode('ascii')
        stream.write(safe)
    try:
        stream.flush()
    except (AttributeError, ValueError):
        pass

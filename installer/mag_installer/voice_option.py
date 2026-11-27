"""Whether a player build includes in-match voice chat, and the player's remembered choice.

Voice chat (docs/voice-chat.md) is compiled out unless a build passes ``-DMAG_VOICE=ON``. A player
build includes it only when the player asks, in any of three ways (the first that says anything
wins):

1. the wizard's **Voice chat** box, or ``build --voice`` / ``build --no-voice``;
2. ``MAG_VOICE=1`` (or ``0``) in the installer's environment;
3. the choice remembered from an earlier build.

A choice made by 1 or 2 is remembered in the installer's per-user cache folder
(``preferences.json``, beside the compile job count; never in a MAG folder), so building again for
a new MAG version keeps it. Off is the default, and choosing off removes the remembered choice, so
with nothing chosen the build, its outputs, its receipts and the preferences file are unchanged.
The build environment itself never inherits MAG_* variables.
"""
from __future__ import annotations

from .build_jobs import SCHEMA, _path, _read
from .model import InstallerError

KEY = 'voice'


def from_environment(environ):
    """MAG_VOICE from the installer's environment: True, False, or None when unset or blank."""
    value = environ.get('MAG_VOICE', '').strip()
    if not value:
        return None
    if value not in ('0', '1'):
        raise InstallerError(f'MAG_VOICE must be 1 (build with voice chat) or 0 (without), not {value!r}')
    return value == '1'


def remembered(cache):
    """True when an earlier build chose voice chat; False otherwise."""
    return _read(cache).get(KEY) is True


def remember(cache, enabled):
    """Keep voice chat on for later builds, or (False) forget it, which is the default."""
    path = _path(cache)
    if path.parent.is_symlink():
        raise InstallerError(f'The installer cache folder must not be a symlink: {path.parent}')
    current = _read(cache)
    if (current.get(KEY) is True) == enabled:
        return   # nothing to change: leave the file as it is
    data = {key: value for key, value in current.items() if key != KEY}
    data['schema_version'] = SCHEMA
    if enabled:
        data[KEY] = True
    path.parent.mkdir(parents=True, exist_ok=True)
    from .pipeline import _write_json
    _write_json(path, data)


def resolve(cache, environ, requested=None):
    """Whether this build includes voice chat, and why: 'chosen', 'environment', 'remembered' or 'default'.

    requested: True or False from the wizard or the command line, or None. A requested or
    environment choice is checked first (nothing is written for an invalid one), then remembered."""
    if requested is not None and type(requested) is not bool:
        raise InstallerError(f'The voice chat choice must be on or off, not {requested!r}')
    environment = from_environment(environ)
    if requested is not None:
        remember(cache, requested)
        return requested, 'chosen'
    if environment is not None:
        remember(cache, environment)
        return environment, 'environment'
    if remembered(cache):
        return True, 'remembered'
    return False, 'default'


def describe(enabled, source):
    """One line for the build log, or None for the default (no voice, nothing chosen)."""
    if source == 'default':
        return None
    if not enabled:
        return 'Building without voice chat (' + ('MAG_VOICE=0' if source == 'environment' else 'your choice') + ').'
    why = {'chosen': 'your choice, remembered for later builds',
           'environment': 'MAG_VOICE=1, remembered for later builds',
           'remembered': 'your earlier choice'}[source]
    return (f'Building with voice chat ({why}). Once MAG is built, turn it on in MAG Settings → General → Voice '
            'and hold K to talk in a match. To build without it again, untick Voice chat, or use build --no-voice '
            'or MAG_VOICE=0.')

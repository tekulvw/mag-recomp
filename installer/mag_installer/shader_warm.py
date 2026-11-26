"""Warm the player's shader cache right after a Windows build (docs/shader-warm.md).

MagShaderWarm, built from the same runtime as the engine, finds the game's RSX programs in the
player's own archives and translates them into %LOCALAPPDATA%\\MAG\\cache\\shaders, the folder the
engine reads (src/mag_shader_cache.h). The first session then loads them instead of translating
them during play. This step never fails an installation: any problem is reported as a warning and
the game translates on first use as before. MAG_SHADER_CACHE=0 skips it, as it turns the cache
off in the game. The files it writes are derived from the player's game files and stay on the
player's computer."""
from __future__ import annotations

import os
import re
import time
from pathlib import Path

from .model import InstallerError
from .pipeline import run_command
from .safety import Cancelled

TARGET = 'MagShaderWarm'
ENGINE_BUILD = 'build-engine'
SUMMARY = re.compile(r'^\[shader-warm\] .*$', re.M)


def applies(config):
    """Windows Vulkan builds: the only ones whose engine keeps the shader cache by default."""
    return 'msvc' in config and bool(config.get('managed_prefix'))


def _summary(log):
    try:
        text = Path(log).read_text(encoding='utf-8', errors='replace')
    except OSError:
        return ''
    return '\n'.join(SUMMARY.findall(text))


def warm(config, workspace, logs, build_env, token, events, environ=None):
    """Build the tool, run it, report. Returns a summary dict; raises only on cancellation."""
    environ = dict(os.environ if environ is None else environ)
    if not applies(config):
        return {'status': 'not-applicable'}
    if environ.get('MAG_SHADER_CACHE') == '0':
        events({'event': 'shader-warm', 'status': 'skipped',
                'message': 'Shader warm-up skipped (MAG_SHADER_CACHE=0).'})
        return {'status': 'skipped'}
    workspace, logs = Path(workspace), Path(logs)
    started = time.monotonic()
    events({'event': 'stage-start', 'stage': 'shader-warm', 'logs': str(logs)})
    log = logs / 'shader-warm.log'
    try:
        run_command([config['cmake'], '--build', str(workspace / ENGINE_BUILD), '--target', TARGET,
                     '--parallel', str(config.get('jobs', 2))],
                    workspace, logs / 'build-shader-warm.log', token, build_env, events, 'build-shader-warm')
        tool = workspace / ENGINE_BUILD / (TARGET + '.exe')
        game = workspace / 'local/retail-v1.0/PS3_GAME'
        # The player's own environment: LOCALAPPDATA and any cache switches the game would see.
        run_command([str(tool), '--game', str(game)], workspace, log, token, environ, events, 'shader-warm')
    except Cancelled:
        raise
    except (InstallerError, OSError) as exc:
        message = (f'Shader warm-up did not finish ({exc}). MAG is installed and translates shaders '
                   'during its first session instead.')
        events({'event': 'shader-warm', 'status': 'warning', 'message': message})
        return {'status': 'warning', 'error': str(exc), 'seconds': round(time.monotonic() - started, 1)}
    seconds = round(time.monotonic() - started, 1)
    summary = _summary(log)
    events({'event': 'shader-warm', 'status': 'done', 'seconds': seconds,
            'message': f'Shader warm-up finished in {seconds} s. {summary}'.strip()})
    return {'status': 'done', 'seconds': seconds, 'summary': summary}

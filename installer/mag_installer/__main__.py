"""Player wizard entry point and scriptable installer commands."""
import argparse
import json
from pathlib import Path
import signal
import sys

from .console import use_utf8_streams, write_line
from .distribution import bundle
from .model import InstallerError, Profile, host_key
from .pipeline import install
from .safety import Cancellation, Cancelled, validate_inputs


def resource_root():
    if getattr(sys, 'frozen', False):
        if not hasattr(sys, '_MEIPASS'):
            raise InstallerError('Frozen launcher resources are unavailable')
        return Path(sys._MEIPASS) / 'resources'
    return Path(__file__).resolve().parents[1]


ROOT = resource_root()
PROFILES = {'mag-unconfigured', 'synthetic-demonstration'}


def emit(event):
    write_line(sys.stdout, json.dumps(event, ensure_ascii=False))


def main(argv=None):
    # Progress text has arrows and dashes that a redirected Windows stream (cp1252) cannot encode.
    use_utf8_streams()
    if sys.version_info < (3, 11):
        print('MAG installer source requires Python 3.11 or newer. Use a supported Python or a bundled player release.', file=sys.stderr)
        return 2
    parser = argparse.ArgumentParser(description='MAG player installer. Open the wizard for automatic tool downloads, local compilation and app packaging. Use wizard --synthetic for the labelled demonstration.')
    def presentation(command, suppress=False):
        mode = command.add_mutually_exclusive_group()
        default = argparse.SUPPRESS if suppress else None
        mode.add_argument('--json', dest='json_mode', action='store_true', default=default, help='Emit machine-readable JSON events')
        mode.add_argument('--human', dest='json_mode', action='store_false', default=default, help='Show readable progress and errors')
    presentation(parser)
    commands = parser.add_subparsers(dest='command')
    wizard = commands.add_parser('wizard', help='Open the guided installer')
    wizard.add_argument('--synthetic', action='store_true', help='Show the labelled demonstration, which does not build MAG')
    presentation(wizard, True)
    player = commands.add_parser('build', help='Automatically download tools, build MAG and package the final app; '
                                 'an existing MAG folder is updated in place')
    presentation(player, True)
    player.add_argument('--source', type=Path, help='Folder containing your supported prepared game files (required for a new '
                        'MAG folder; an update uses the remembered one when it is left out)')
    target = player.add_mutually_exclusive_group(required=True)
    target.add_argument('--parent', type=Path, help='Existing folder in which to create (or update) the MAG installation')
    target.add_argument('--output', type=Path, help='Exact MAG installation folder: a new one, or an existing one to update')
    player.add_argument('--firmware', type=Path, help='The decrypted firmware module (libsre.prx) to use, for example the one '
                        'RPCS3\'s --decrypt wrote beside its libsre.sprx; without it, firmware/libsre.prx next to PS3_GAME '
                        '(an update first uses the copy the installer keeps)')
    player.add_argument('--retry', action='store_true', help='Retry a matching failed or cancelled build in the same folder, retaining earlier logs')
    player.add_argument('--update', action='store_true', help='Require an existing MAG folder and update it in place '
                        '(the default whenever the folder already holds MAG)')
    player.add_argument('--cache', type=Path, help='Optional download cache override')
    player.add_argument('--jobs', metavar='N', help='How many compile jobs run at once: a whole number from 1 to 64, or auto for '
                        'this computer\'s default (set by its memory and CPUs). Remembered for later builds; '
                        'works with --retry. Without it, the remembered choice or the default is used')
    player.add_argument('--voice', action=argparse.BooleanOptionalAction, default=None,
                        help='Build with (--voice) or without (--no-voice) in-match voice chat, push to talk. Remembered for '
                             'later builds. Without either, MAG_VOICE=1 or 0 in the environment, else (an update) the '
                             'option the MAG being updated was built with, else the remembered choice, else without voice chat')
    upgrade = commands.add_parser('update', help='Update an existing MAG folder in place: same paths, saves, settings and logs '
                                  'kept, the previous version kept in MAG-build/previous')
    presentation(upgrade, True)
    target = upgrade.add_mutually_exclusive_group(required=True)
    target.add_argument('--parent', type=Path, help='Folder holding the MAG folder to update')
    target.add_argument('--output', type=Path, help='The MAG folder to update')
    upgrade.add_argument('--source', type=Path, help='Your game folder; without it, the one used last time, else the MAG '
                         'folder\'s own game files, else the one RPCS3 holds')
    upgrade.add_argument('--firmware', type=Path, help='The decrypted firmware module (libsre.prx); without it, the copy the '
                         'installer keeps, else the one used last time, else the one RPCS3 holds')
    upgrade.add_argument('--restore-previous', action='store_true', help='Put back the version the last update replaced '
                         '(no build); the newer one is kept in its place')
    upgrade.add_argument('--cache', type=Path, help='Optional download cache override')
    upgrade.add_argument('--jobs', metavar='N', help='How many compile jobs run at once (1 to 64, or auto)')
    upgrade.add_argument('--voice', action=argparse.BooleanOptionalAction, default=None,
                         help='Build with or without voice chat; without either, the option this MAG was built with')
    guide = commands.add_parser('guide', help='Find your RPCS3 files and MAG game folder, check them, and show what is left to prepare')
    presentation(guide, True)
    guide.add_argument('--source', type=Path, help='Check this game folder instead of the one found')
    guide.add_argument('--firmware', type=Path, help='Check this firmware module (libsre.prx) instead of the one found')
    guide.add_argument('--time-limit', type=float, default=None, help='Seconds to spend looking for RPCS3 (default 3)')
    reuse = commands.add_parser('build-cache', help='Show, limit or clear the build cache that lets later builds reuse earlier work')
    presentation(reuse, True)
    reuse.add_argument('--cache', type=Path, help='Installer cache folder (defaults to your user cache)')
    action = reuse.add_mutually_exclusive_group()
    action.add_argument('--clear', action='store_true', help='Delete the build cache (downloads are kept); the next build does all of its work again')
    action.add_argument('--max-size', help='Limit the build cache, for example 20GiB (the default) or 10GB')
    for name in ('status', 'validate', 'preflight', 'install'):
        command = commands.add_parser(name)
        presentation(command, True)
        command.add_argument('--profile', choices=sorted(PROFILES))
        if name != 'status':
            command.add_argument('--source', type=Path, required=True, help='Directory containing your own input files; see the profile for required identities')
        if name in ('preflight', 'install'):
            output = command.add_mutually_exclusive_group(required=True)
            output.add_argument('--output', type=Path, help='Exact new destination directory; must not already exist')
            output.add_argument('--parent', type=Path, help='Existing installation parent; creates a new MAG (or MAG Demo) child')
            command.add_argument('--cache', type=Path, help='Managed tool cache directory (defaults to your user cache)')
            command.add_argument('--synthetic', action='store_true', help='Run the labelled synthetic demonstration only')
        if name == 'install':
            command.add_argument('--resume', action='store_true', help='Explicitly retry a matching interrupted transaction; revalidates inputs and replays stages')
            command.add_argument('--keep-work', action='store_true', help='Keep owned snapshots and build work after success; logs and journal are always retained')
    for name in ('native-preflight', 'native-build'):
        command = commands.add_parser(name, help='Experimental local source/toolchain build; no automatic source download or game launch')
        presentation(command, True)
        command.add_argument('--config', type=Path, required=True, help='Explicit trusted LOCAL recipe JSON; uses your existing source, SDK and decrypted inputs')
        command.add_argument('--output', type=Path, required=True, help='NEW retained build workspace; must not already exist, even after failure')
    distribution = commands.add_parser('bundle', help='Create a tooling-only SOURCE archive, not a player-ready release')
    presentation(distribution, True)
    distribution.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    json_mode = args.json_mode if args.json_mode is not None else not sys.stdout.isatty()
    def report(event, error=False):
        stream = sys.stderr if error else sys.stdout
        if json_mode:
            write_line(stream, json.dumps(event, ensure_ascii=False))
        else:
            from .ux import format_event
            if 'platforms' in event:
                # Build MAG is the player route. The profile below is the older per-host
                # installation model, which never installs MAG, so say which is which.
                build = event.get('player_build') or {}
                message = (f"Build MAG: ready on {event['host']}." if build.get('ready') else
                           f"Build MAG: not available on {event['host']}: {build.get('reason') or 'see status --json'}")
                message += f"\nLegacy {event['profile']} profile routes (not used by Build MAG):"
                message += '\n' + '\n'.join(f"{key}: {value.get('status', 'unknown')} — {value.get('reason', value.get('blockers', 'See status --json for details'))}" for key, value in event['platforms'].items())
            elif event.get('event') == 'preflight':
                from .ux import space_summary
                message = space_summary(event)
            else:
                message = format_event(event)
                if event.get('event') in ('synthetic-complete', 'local-build-complete', 'experimental-local-build-complete', 'player-build-complete', 'tooling-bundle-created'):
                    for key in ('output', 'logs', 'path'):
                        if event.get(key):
                            message += f"\n{key.capitalize()}: {event[key]}"
            write_line(stream, message)
    token = Cancellation()
    handlers = {}
    try:
        if args.command in (None, 'wizard'):
            try:
                from .gui import launch
                return launch(ROOT, synthetic=getattr(args, 'synthetic', False))
            except ImportError as exc:
                raise InstallerError('The graphical installer requires Python with Tcl/Tk. Use --help for CLI commands, or a player release with the bundled GUI runtime.') from exc
        if args.command == 'bundle':
            path = bundle(ROOT, args.output)
            report({'event': 'tooling-bundle-created', 'path': str(path), 'release_ready': False})
            return 0
        if args.command == 'build':
            from . import player_build
            from .ux import default_cache, destination
            for signum in (signal.SIGINT, signal.SIGTERM):
                handlers[signum] = signal.signal(signum, lambda number, frame: token.cancel())
            output = args.output if args.output is not None else destination(args.parent)
            cache = args.cache or default_cache()
            if args.update or player_build.update_plan(ROOT, output, cache) is not None:
                # An existing MAG folder: update it in place instead of refusing it.
                player_build.update(ROOT, output, cache, source=args.source, firmware=args.firmware, token=token,
                                    events=report, jobs=args.jobs, voice=args.voice)
                return 0
            if args.source is None:
                raise InstallerError('Choose your game folder with --source to build a new MAG folder.')
            player_build.install(ROOT, args.source, output, cache, token=token, events=report, retry=args.retry,
                                 jobs=args.jobs, **({'voice': args.voice} if args.voice is not None else {}),
                                 **({'firmware': args.firmware} if args.firmware is not None else {}))
            return 0
        if args.command == 'update':
            from . import player_build
            from .ux import default_cache, destination
            for signum in (signal.SIGINT, signal.SIGTERM):
                handlers[signum] = signal.signal(signum, lambda number, frame: token.cancel())
            output = args.output if args.output is not None else destination(args.parent)
            player_build.update(ROOT, output, args.cache or default_cache(), source=args.source, firmware=args.firmware,
                                token=token, events=report, jobs=args.jobs, voice=args.voice,
                                restore_previous=args.restore_previous)
            return 0
        if args.command == 'guide':
            from . import input_guide, rpcs3_detect
            for signum in (signal.SIGINT, signal.SIGTERM):
                handlers[signum] = signal.signal(signum, lambda number, frame: token.cancel())
            detection = rpcs3_detect.detect(**({'time_limit': args.time_limit} if args.time_limit is not None else {}))
            reviewed = input_guide.load_reviewed(ROOT)
            steps, game, firmware, game_path, firmware_path = input_guide.survey(detection, reviewed, args.source, args.firmware, token)
            ready = all(step.state == input_guide.DONE for step in steps)
            event = {'event': 'guide', 'ready': ready, 'reviewed_identities': reviewed is not None,
                     'source': str(game_path) if game_path else None, 'firmware': str(firmware_path) if firmware_path else None,
                     'steps': [{'number': step.number, 'title': step.title, 'state': step.state, 'lines': step.lines,
                                'commands': step.commands, 'link': step.link} for step in steps],
                     'detection': detection.to_json(),
                     'message': input_guide.render(steps, detection) + ('\n\nThen click Build MAG in the installer window, or run the installer with these arguments:\n  '
                                   + input_guide.suggested_build(detection, game_path, firmware_path))}
            report(event)
            return 0
        if args.command == 'build-cache':
            from . import build_cache
            from .ux import default_cache
            store = build_cache.BuildCache.for_cache(args.cache or default_cache(), token, report)
            if args.clear:
                freed = store.clear()
                report({'event': 'build-cache-cleared', 'freed_bytes': freed, 'path': str(store.root),
                        'message': f'Build cache cleared ({build_cache.size_text(freed)} freed). Downloads were kept.'})
            else:
                if args.max_size:
                    store.set_max_bytes(build_cache.parse_size(args.max_size))
                report({'event': 'build-cache-usage', **store.usage()})
            return 0
        if args.command in ('native-preflight', 'native-build'):
            try:
                from . import native_build
            except ImportError as exc:
                raise InstallerError('Experimental local build support is unavailable in this installer') from exc
            for signum in (signal.SIGINT, signal.SIGTERM):
                handlers[signum] = signal.signal(signum, lambda number, frame: token.cancel())
            if args.command == 'native-preflight':
                report({'event': 'native-preflight', **native_build.preflight(args.config, args.output, token=token)})
            else:
                native_build.install(args.config, args.output, resume=False, token=token, events=report)
            return 0
        profile_id = args.profile or ('synthetic-demonstration' if getattr(args, 'synthetic', False) else 'mag-unconfigured')
        profile = Profile.load(ROOT / 'profiles' / f'{profile_id}.json')
        if args.command == 'status':
            from .player_build import readiness as player_readiness
            report({'profile': profile.data['id'], 'revision': profile.data['revision'],
                    'kind': profile.data['kind'], 'profile_sha256': profile.digest,
                    'host': host_key(), 'release_ready': False, 'platforms': profile.data['platforms'],
                    'player_build': player_readiness(ROOT)})
            return 0
        profile.adapter()
        if args.command in ('install', 'preflight'):
            from .ux import default_cache, destination
            output = args.output if args.output is not None else destination(args.parent, synthetic=args.synthetic)
            cache = args.cache if args.cache is not None else default_cache()
        if args.command == 'preflight':
            from .pipeline import preflight
            report({'event': 'preflight', **preflight(profile, args.source, output, cache)})
            return 0
        for signum in (signal.SIGINT, signal.SIGTERM):
            handlers[signum] = signal.signal(signum, lambda number, frame: token.cancel())
        if args.command == 'validate':
            identity = validate_inputs(profile, args.source, token)
            report({'event': 'input-validated', 'kind': profile.data['kind'], 'identity': identity})
        else:
            install(profile, args.source, output, cache, args.resume, args.synthetic, token, report, keep_work=args.keep_work)
        return 0
    except Cancelled as exc:
        report({'event': 'cancelled', 'message': str(exc)}, error=True)
        return 130
    except (InstallerError, OSError) as exc:
        report({'event': 'error', 'message': str(exc)}, error=True)
        return 2
    finally:
        for signum, handler in handlers.items(): signal.signal(signum, handler)


if __name__ == '__main__': sys.exit(main())

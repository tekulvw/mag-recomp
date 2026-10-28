"""Player wizard entry point and scriptable installer commands."""
import argparse
import json
from pathlib import Path
import signal
import sys

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
    print(json.dumps(event, ensure_ascii=False), flush=True)


def main(argv=None):
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
    player = commands.add_parser('build', help='Automatically download tools, build MAG and package the final app')
    presentation(player, True)
    player.add_argument('--source', type=Path, required=True, help='Folder containing your supported prepared game files')
    target = player.add_mutually_exclusive_group(required=True)
    target.add_argument('--parent', type=Path, help='Existing folder in which to create the MAG installation')
    target.add_argument('--output', type=Path, help='Exact new MAG installation folder')
    player.add_argument('--retry', action='store_true', help='Retry a matching failed or cancelled build in the same folder, retaining earlier logs')
    player.add_argument('--cache', type=Path, help='Optional download cache override')
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
            print(json.dumps(event, ensure_ascii=False), file=stream, flush=True)
        else:
            from .ux import format_event
            if 'platforms' in event:
                message = f"{event['profile']}: real MAG installation is blocked; host {event['host']}."
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
            print(message, file=stream, flush=True)
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
            player_build.install(ROOT, args.source, output, args.cache or default_cache(), token=token, events=report, retry=args.retry)
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

"""Small Tk wizard with a headless, queue-driven transaction controller."""
from __future__ import annotations

from pathlib import Path
import os
import json
import queue
import subprocess
import sys
import threading

from .model import InstallerError, Profile
from .safety import Cancellation, Cancelled
from . import ux
from .native_build import FIRMWARE_SOURCE_HINT
from .player_build import BUILD_SUFFIX


class WizardController:
    def __init__(self, profile, synthetic=False, preflight=None, installer=None):
        self.profile, self.synthetic = profile, synthetic
        self._preflight, self._installer = preflight, installer
        self.events = queue.Queue()
        self.worker = None
        self.state = 'idle'
        self.error = ''
        self.summary = None
        self.output = self.logs = None
        self.expected_input = profile.data['identity']['label']
        self.blocked = ''
        try:
            profile.adapter()
            if (profile.data['kind'] == 'synthetic') != synthetic:
                raise InstallerError('Synthetic demonstrations require an explicit demonstration launch')
        except InstallerError as exc:
            self.blocked = str(exc)
            self.state = 'blocked'

    @property
    def notice(self):
        if self.blocked:
            return 'This preview cannot install MAG yet. The game build recipe is not available for this platform.'
        return 'Choose your input folder and where the new output folder should be created.'

    @property
    def busy(self):
        return self.worker is not None and self.worker.is_alive()

    def display_output(self, parent):
        if (self.busy or self.state == 'complete') and self.output is not None:
            return self.output
        return ux.destination(parent, self.synthetic)

    def has_journal(self, parent):
        output = ux.destination(parent, self.synthetic)
        journal = output.parent / f'.{output.name}.mag-staging' / 'journal.json'
        if not journal.exists():
            return False
        try:
            data = json.loads(journal.read_text(encoding='utf-8'))
            return not isinstance(data, dict) or data.get('state') != 'complete'
        except (OSError, ValueError, UnicodeError):
            # Unknown recovery data never grants permission to overwrite it.
            # Explicit retry still delegates journal validation to the engine.
            return True

    def start(self, source, parent, cache, install=False, resume=False):
        if self.blocked:
            raise InstallerError(self.blocked)
        if self.busy:
            raise InstallerError('An operation is already running')
        output = ux.destination(parent, self.synthetic).absolute()
        if output.exists() or output.is_symlink():
            raise InstallerError('Output already exists. Open the folder or choose another installation parent; its contents have not been verified.')
        if install and self.has_journal(parent) and not resume:
            raise InstallerError('Interrupted work exists. Choose explicit retry to reverify inputs and replay all stages.')
        self.output = output
        self.logs = output.parent / f'.{output.name}.mag-staging' / 'logs'
        self.error = ''
        self.token = Cancellation()
        self.state = 'installing' if install else 'checking'
        def run():
            try:
                from . import pipeline
                check = self._preflight or pipeline.preflight
                self.token.check()
                summary = check(self.profile, Path(source), output, Path(cache))
                self.token.check()
                if install:
                    build = self._installer or pipeline.install
                    result = build(self.profile, Path(source), output, Path(cache), resume=resume,
                                   synthetic=self.synthetic, token=self.token, events=self.events.put)
                    self.events.put({'event': '_complete', 'output': str(result or output)})
                else:
                    self.events.put({'event': '_ready', 'summary': summary})
            except Cancelled as exc:
                self.events.put({'event': '_cancelled', 'message': str(exc)})
            except Exception as exc:
                self.events.put({'event': '_failed', 'message': str(exc) or type(exc).__name__})
        self.worker = threading.Thread(target=run, name='mag-installer-worker', daemon=False)
        self.worker.start()

    def poll(self):
        result = []
        while True:
            try:
                event = self.events.get_nowait()
            except queue.Empty:
                break
            result.append(event)
            name = event.get('event')
            if name == '_ready':
                self.state, self.summary = 'ready', event['summary']
            elif name == '_complete':
                self.state = 'complete'
                self.output = Path(event['output'])
            elif name in ('_failed', '_cancelled'):
                self.state = name[1:]
                self.error = event['message']
            if event.get('logs'):
                self.logs = Path(event['logs'])
            elif event.get('log'):
                self.logs = Path(event['log']).parent
        return result

    def cancel(self):
        if self.busy:
            self.token.cancel()

    def request_close(self):
        if self.busy:
            self.cancel()
            return False
        return True


def visible_progress_event(event):
    # The controller confirms the returned transaction exactly once below.
    return event.get('event') not in ('synthetic-complete', 'local-build-complete', 'experimental-local-build-complete', 'player-build-complete')


def open_folder(path):
    path = Path(path)
    if not path.is_dir():
        raise InstallerError(f'Folder does not exist yet: {path}')
    if sys.platform == 'win32':
        os.startfile(str(path))
    else:
        subprocess.Popen(['open' if sys.platform == 'darwin' else 'xdg-open', str(path)],
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class PlayerBuildController(WizardController):
    """One player action owns prerequisites through final application output."""
    def __init__(self, root, readiness=None, installer=None):
        self.root = Path(root)
        self._installer = installer
        self.events = queue.Queue()
        self.worker = None
        self.state, self.error = 'idle', ''
        self.summary = self.output = self.logs = None
        self.synthetic = False
        try:
            if readiness is None:
                from . import player_build
                readiness = player_build.readiness
            report = readiness(self.root)
            self.blocked = '' if report.get('ready') else report.get('reason', 'The maintained MAG build kit is unavailable.')
        except (ImportError, InstallerError, OSError) as exc:
            self.blocked = f'The maintained MAG build kit is unavailable: {exc}'
        if self.blocked:
            self.state = 'blocked'

    def has_journal(self, parent):
        """A failed build to retry: in the build folder, or inside a MAG folder of the earlier layout."""
        from .player_build import build_folder
        output = ux.destination(parent)
        for folder in (build_folder(output), output):
            try:
                journal = folder / 'build.json'
                if folder.is_symlink() or journal.is_symlink() or not journal.is_file():
                    continue
                data = json.loads(journal.read_text(encoding='utf-8'))
                if isinstance(data, dict) and data.get('schema_version') == 2 and data.get('state') in ('incomplete', 'building'):
                    return True
            except (OSError, ValueError):
                continue
        return False

    def legacy_install(self, parent):
        """A finished MAG folder an earlier installer built with its build files inside it."""
        from .player_build import legacy_journal
        status = legacy_journal(ux.destination(parent))
        return status is not None and status.get('state') == 'complete'

    def build_cache_usage(self, cache):
        from . import build_cache
        return build_cache.usage(cache)

    def clear_build_cache(self, cache):
        """Delete the per-user build cache (never while this installer is building); bytes freed."""
        if self.busy:
            raise InstallerError('Wait for the build to finish before clearing the build cache.')
        from . import build_cache
        return build_cache.clear(cache)

    def jobs_choice(self, cache):
        """What the Compile jobs field shows: the remembered choice, else this computer's default."""
        from . import build_jobs
        cpus, memory = build_jobs.logical_cpus(), build_jobs.total_memory()
        default = build_jobs.recommended(cpus, memory)
        remembered = build_jobs.remembered(cache)
        return {'value': default if remembered is None else remembered, 'default': default, 'remembered': remembered,
                'minimum': build_jobs.MIN_JOBS, 'maximum': build_jobs.MAX_JOBS, 'cpus': cpus, 'memory': memory}

    def jobs_hint(self, choice):
        from . import build_jobs
        computer = (f'{choice["cpus"]} logical CPUs, {choice["memory"] / build_jobs.GIB:.0f} GiB memory'
                    if choice['cpus'] and choice['memory'] else 'its CPUs and memory could not be read')
        return (f'How many compiles run at once ({choice["minimum"]}–{choice["maximum"]}). Default for this computer: '
                f'{choice["default"]} ({computer}). More jobs build faster; allow about '
                f'{build_jobs.JOB_MEMORY // build_jobs.GIB} GiB of memory for each. Your choice is remembered for later builds.')

    def jobs_request(self, text, default):
        """The field's text as a build's jobs argument: the default means 'auto' (follow this computer)."""
        from . import build_jobs
        jobs = build_jobs.parse(text)
        return build_jobs.AUTO if jobs == default else jobs

    def voice_choice(self, cache, environ=None):
        """What the Voice chat box shows: MAG_VOICE when set, else the remembered choice (off by default)."""
        from . import voice_option
        try:
            environment = voice_option.from_environment(os.environ if environ is None else environ)
        except InstallerError:
            environment = None   # Build MAG reports the invalid value before anything starts
        return environment if environment is not None else voice_option.remembered(cache)

    VOICE_HINT = ('Builds in-match voice chat (push to talk, being tested). After the build, turn it on in MAG Settings '
                  '→ General → Voice and hold K to talk. Your choice is remembered for later builds.')

    def start(self, source, parent, cache, retry=False, jobs=None, voice=None):
        if self.blocked:
            raise InstallerError(self.blocked)
        if self.busy:
            raise InstallerError('A build is already running')
        if jobs is not None and str(jobs).strip().lower() != 'auto':
            from .build_jobs import parse
            jobs = parse(jobs)  # a clear error before anything starts
        from .player_build import build_folder
        output = ux.destination(parent).absolute()
        if (output.exists() or output.is_symlink()) and not retry and not self.legacy_install(parent):
            raise InstallerError('Output already exists; choose another installation parent. Existing files are preserved.')
        self.output, self.logs = output, build_folder(output)/'logs'
        self.token = Cancellation()
        self.error = ''
        self.state = 'installing'
        def run():
            try:
                build = self._installer
                if build is None:
                    from . import player_build
                    build = player_build.install
                self.token.check()
                result = build(self.root, Path(source).expanduser(), output, Path(cache).expanduser(), token=self.token, events=self.events.put,
                               **({'retry': True} if retry else {}), **({'jobs': jobs} if jobs is not None else {}),
                               **({'voice': voice} if voice is not None else {}))
                self.events.put({'event':'_complete','output':str(result or output)})
            except Cancelled as exc:
                self.events.put({'event':'_cancelled','message':str(exc)})
            except Exception as exc:
                self.events.put({'event':'_failed','message':str(exc) or type(exc).__name__})
        self.worker = threading.Thread(target=run,name='mag-player-build-worker',daemon=False)
        self.worker.start()


def launch(root: Path, synthetic: bool = False) -> int:
    try:
        import tkinter as tk
        from tkinter import ttk, filedialog, messagebox
        window = tk.Tk()
    except Exception as exc:
        print(f'Cannot open the installer window: {exc}. Use a Python runtime with matching Tcl/Tk support, or --help for CLI commands. Bundled installers must include Tcl/Tk.',file=sys.stderr)
        return 2
    if synthetic:
        try:
            controller = WizardController(Profile.load(Path(root)/'profiles/synthetic-demonstration.json'),True)
        except (InstallerError,OSError) as exc:
            window.destroy();print(str(exc),file=sys.stderr);return 2
    else:
        controller = PlayerBuildController(root)
    window.title('MAG Installer' + (' — Synthetic demonstration' if synthetic else ''))
    window.geometry('820x700');window.minsize(680,600)
    frame=ttk.Frame(window,padding=24);frame.pack(fill='both',expand=True)
    frame.columnconfigure(1,weight=1);frame.rowconfigure(10,weight=1)
    ttk.Label(frame,text='Installer demonstration' if synthetic else 'Build MAG',font=('TkDefaultFont',22,'bold')).grid(row=0,column=0,columnspan=3,sticky='w',pady=(0,12))
    intro=('SYNTHETIC ONLY. Uses sample text; no game is built or playable.' if synthetic else 'Choose your game files and installation location. Build MAG handles the required tools and build steps automatically.')
    ttk.Label(frame,text=intro,wraplength=760).grid(row=1,column=0,columnspan=3,sticky='w',pady=(0,12))
    notice=controller.blocked or ('Input needs SYNTHETIC-INPUT.txt containing “This is synthetic installer input.” followed by one LF newline.' if synthetic else 'Select the folder containing PS3_GAME, prepared game executables and firmware. Required tools download automatically. '+FIRMWARE_SOURCE_HINT)
    ttk.Label(frame,text=notice,wraplength=760).grid(row=2,column=0,columnspan=3,sticky='w',pady=(0,12))
    source=tk.StringVar();parent=tk.StringVar(value=str(ux.default_parent()))
    controls=[]
    def browse(variable):
        chosen=filedialog.askdirectory(parent=window,initialdir=variable.get() or str(Path.home()))
        if chosen:variable.set(chosen)
    for row,label,variable in ((3,'Game folder' if not synthetic else 'Sample input folder',source),(4,'Install inside',parent)):
        ttk.Label(frame,text=label).grid(row=row,column=0,sticky='w',padx=(0,12),pady=6)
        entry=ttk.Entry(frame,textvariable=variable);entry.grid(row=row,column=1,sticky='ew',pady=6)
        button=ttk.Button(frame,text='Browse…',command=lambda v=variable:browse(v));button.grid(row=row,column=2,padx=(8,0))
        controls.extend((entry,button))
    jobs=jobs_choice=voice=voice_initial=None
    if not synthetic:
        # Build option: how many compile jobs run at once (build_jobs.py), remembered between builds.
        jobs_choice=controller.jobs_choice(ux.default_cache())
        jobs=tk.StringVar(value=str(jobs_choice['value']))
        ttk.Label(frame,text='Compile jobs').grid(row=5,column=0,sticky='nw',padx=(0,12),pady=6)
        jobs_row=ttk.Frame(frame);jobs_row.grid(row=5,column=1,columnspan=2,sticky='ew',pady=6);jobs_row.columnconfigure(1,weight=1)
        spin=ttk.Spinbox(jobs_row,from_=jobs_choice['minimum'],to=jobs_choice['maximum'],increment=1,width=5,textvariable=jobs)
        spin.grid(row=0,column=0,sticky='nw')
        ttk.Label(jobs_row,text=controller.jobs_hint(jobs_choice),wraplength=600).grid(row=0,column=1,sticky='w',padx=(12,0))
        controls.append(spin)
        # Build option: in-match voice chat (voice_option.py), off by default, remembered between builds.
        voice_initial=controller.voice_choice(ux.default_cache())
        voice=tk.BooleanVar(value=voice_initial)
        ttk.Label(frame,text='Voice chat').grid(row=6,column=0,sticky='nw',padx=(0,12),pady=6)
        voice_row=ttk.Frame(frame);voice_row.grid(row=6,column=1,columnspan=2,sticky='ew',pady=6);voice_row.columnconfigure(1,weight=1)
        voice_box=ttk.Checkbutton(voice_row,text='Include',variable=voice)
        voice_box.grid(row=0,column=0,sticky='nw')
        ttk.Label(voice_row,text=controller.VOICE_HINT,wraplength=600).grid(row=0,column=1,sticky='w',padx=(12,0))
        controls.append(voice_box)
    destination_label=ttk.Label(frame,wraplength=760);destination_label.grid(row=7,column=0,columnspan=3,sticky='w',pady=8)
    status=tk.StringVar(value=controller.blocked or 'Ready when you are.')
    ttk.Label(frame,textvariable=status,wraplength=760).grid(row=8,column=0,columnspan=3,sticky='w',pady=8)
    progress=ttk.Progressbar(frame,mode='indeterminate');progress.grid(row=9,column=0,columnspan=3,sticky='ew',pady=(0,10))
    details=tk.Text(frame,height=9,wrap='word',state='disabled');details.grid(row=10,column=0,columnspan=3,sticky='nsew')
    actions=ttk.Frame(frame);actions.grid(row=11,column=0,columnspan=3,sticky='ew',pady=(16,0))
    closing=False
    def append(text):
        details.configure(state='normal');details.insert('end',text+'\n');details.see('end');details.configure(state='disabled')
    def start():
        try:
            if not source.get().strip() or not parent.get().strip():raise InstallerError('Choose your game folder and installation location.')
            if synthetic:
                resume=controller.has_journal(Path(parent.get()).expanduser())
                if resume and not messagebox.askyesno('Retry demonstration?', 'Interrupted demonstration work exists. Reverify input and replay its stages?',parent=window):return
                controller.start(Path(source.get()).expanduser(),Path(parent.get()).expanduser(),ux.default_cache(),install=True,resume=resume)
            else:
                controller.start(Path(source.get()).expanduser(),Path(parent.get()).expanduser(),ux.default_cache(),retry=controller.has_journal(Path(parent.get()).expanduser()),
                                 jobs=controller.jobs_request(jobs.get(),jobs_choice['default']),
                                 voice=(voice.get() if voice.get()!=voice_initial else None))
            progress.start(12);refresh()
        except (InstallerError,OSError) as exc:status.set(str(exc))
    def folder(path):
        try:open_folder(path)
        except (InstallerError,OSError) as exc:messagebox.showerror('Cannot open folder',str(exc),parent=window)
    build=ttk.Button(actions,text='Run demonstration' if synthetic else 'Build MAG',command=start);build.pack(side='left')
    cancel=ttk.Button(actions,text='Cancel',command=controller.cancel);cancel.pack(side='left',padx=8)
    open_output=ttk.Button(actions,text='Open output',command=lambda:folder(controller.display_output(Path(parent.get()).expanduser())));open_output.pack(side='right')
    open_logs=ttk.Button(actions,text='Open logs',command=lambda:folder(controller.logs));open_logs.pack(side='right',padx=8)
    clear_cache=None
    if not synthetic:
        def clear_build_cache():
            from .build_cache import size_text
            try:
                held=controller.build_cache_usage(ux.default_cache())['bytes']
                if not held:
                    status.set('The build cache is empty.');return
                if not messagebox.askyesno('Clear build cache?',
                        f'The build cache holds {size_text(held)} of tools, generated sources and compiled objects from earlier builds '
                        'on this computer, so the next build can reuse them. They are made from your own game files and never leave '
                        'this computer. Clearing frees the space; the next build then does all of its work again. Downloads are kept.',parent=window):return
                status.set(f'Build cache cleared ({size_text(controller.clear_build_cache(ux.default_cache()))} freed).')
            except (InstallerError,OSError) as exc:status.set(str(exc))
        clear_cache=ttk.Button(actions,text='Clear build cache…',command=clear_build_cache);clear_cache.pack(side='right',padx=8)
    def refresh():
        for widget in controls:widget.configure(state='disabled' if controller.busy else 'normal')
        build.configure(state='disabled' if controller.busy or controller.blocked else 'normal')
        legacy=False
        if not synthetic:
            build.configure(text='Retry build' if controller.has_journal(Path(parent.get()).expanduser()) else 'Build MAG')
            legacy=not controller.busy and controller.legacy_install(Path(parent.get()).expanduser())
        cancel.configure(state='normal' if controller.busy else 'disabled')
        if clear_cache is not None:clear_cache.configure(state='disabled' if controller.busy else 'normal')
        output=controller.display_output(Path(parent.get()).expanduser())
        destination_label.configure(text=(f'{output} was built by an earlier installer. Build MAG moves its build files beside it, to {output}{BUILD_SUFFIX}, without rebuilding.'
                                           if legacy else f'New output folder: {output}' + ('' if synthetic else f' (build files: {output}{BUILD_SUFFIX})')))
        open_output.configure(state='normal' if output.is_dir() else 'disabled')
        open_logs.configure(state='normal' if controller.logs and controller.logs.is_dir() else 'disabled')
    def tick():
        for event in controller.poll():
            name=event.get('event','')
            if name=='_complete':
                status.set('Synthetic demonstration complete. No MAG application was built.' if synthetic else 'Build finished. Open output to inspect the application. No game was launched.')
                append(status.get()+f'\nOutput: {controller.output}\nLogs: {controller.logs}')
            elif name in ('_failed','_cancelled'):
                status.set(controller.error);append(controller.error+f'\nLogs: {controller.logs}')
            elif visible_progress_event(event):
                status.set(ux.format_event(event));append(status.get())
        if not controller.busy:
            progress.stop()
            if closing:window.destroy();return
        refresh();window.after(100,tick)
    def close():
        nonlocal closing
        if controller.request_close():window.destroy()
        else:closing=True;status.set('Cancelling safely; waiting for the worker to stop…')
    def resize(event):
        for widget in frame.winfo_children():
            if isinstance(widget,ttk.Label):widget.configure(wraplength=max(240,event.width-48))
    frame.bind('<Configure>',resize);window.protocol('WM_DELETE_WINDOW',close)
    if controller.blocked:append(controller.blocked)
    refresh();window.after(100,tick);window.mainloop()
    return 0


class LocalBuildController(WizardController):
    """Explicit trusted local recipe route; retained work is never overwritten."""
    def __init__(self, preflight=None, installer=None):
        self._preflight, self._installer = preflight, installer
        self.events = queue.Queue()
        self.worker = None
        self.state, self.error = 'idle', ''
        self.summary = self.output = self.logs = None

    def start(self, config, output, install=False):
        if self.busy:
            raise InstallerError('An operation is already running')
        config, output = Path(config).expanduser().absolute(), Path(output).expanduser().absolute()
        if output.exists() or output.is_symlink():
            raise InstallerError('Workspace already exists. Its files are retained; choose a new destination for another build.')
        self.output, self.logs = output, output/'logs'
        self.token = Cancellation()
        self.error = ''
        self.state = 'installing' if install else 'checking'
        def run():
            try:
                if (not install and self._preflight is None) or (install and self._installer is None):
                    try:
                        from . import native_build
                    except ImportError as exc:
                        raise InstallerError('The experimental local build module is unavailable in this installer.') from exc
                self.token.check()
                if install:
                    build = self._installer or native_build.install
                    result = build(config, output, resume=False, token=self.token, events=self.events.put)
                    self.events.put({'event':'_complete','output':str(result or output)})
                else:
                    check = self._preflight or native_build.preflight
                    summary = check(config, output, token=self.token)
                    self.token.check()
                    self.events.put({'event':'_ready','summary':summary})
            except Cancelled as exc:
                self.events.put({'event':'_cancelled','message':str(exc)})
            except Exception as exc:
                self.events.put({'event':'_failed','message':str(exc) or type(exc).__name__})
        self.worker = threading.Thread(target=run,name='mag-local-build-worker',daemon=False)
        self.worker.start()


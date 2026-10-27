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

    def start(self, source, parent, cache):
        if self.blocked:
            raise InstallerError(self.blocked)
        if self.busy:
            raise InstallerError('A build is already running')
        output = ux.destination(parent).absolute()
        if output.exists() or output.is_symlink():
            raise InstallerError('Output already exists; choose another installation parent. Existing files are preserved.')
        self.output, self.logs = output, output/'logs'
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
                result = build(self.root, Path(source).expanduser(), output, Path(cache).expanduser(), token=self.token, events=self.events.put)
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
    window.geometry('820x650');window.minsize(680,550)
    frame=ttk.Frame(window,padding=24);frame.pack(fill='both',expand=True)
    frame.columnconfigure(1,weight=1);frame.rowconfigure(8,weight=1)
    ttk.Label(frame,text='Installer demonstration' if synthetic else 'Build MAG',font=('TkDefaultFont',22,'bold')).grid(row=0,column=0,columnspan=3,sticky='w',pady=(0,12))
    intro=('SYNTHETIC ONLY. Uses sample text; no game is built or playable.' if synthetic else 'Choose your game files and installation location. Build MAG handles the required tools and build steps automatically.')
    ttk.Label(frame,text=intro,wraplength=760).grid(row=1,column=0,columnspan=3,sticky='w',pady=(0,12))
    notice=controller.blocked or ('Input needs SYNTHETIC-INPUT.txt containing “This is synthetic installer input.” followed by one LF newline.' if synthetic else 'Select the folder containing PS3_GAME, prepared game executables and firmware. Required tools download automatically.')
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
    destination_label=ttk.Label(frame,wraplength=760);destination_label.grid(row=5,column=0,columnspan=3,sticky='w',pady=8)
    status=tk.StringVar(value=controller.blocked or 'Ready when you are.')
    ttk.Label(frame,textvariable=status,wraplength=760).grid(row=6,column=0,columnspan=3,sticky='w',pady=8)
    progress=ttk.Progressbar(frame,mode='indeterminate');progress.grid(row=7,column=0,columnspan=3,sticky='ew',pady=(0,10))
    details=tk.Text(frame,height=9,wrap='word',state='disabled');details.grid(row=8,column=0,columnspan=3,sticky='nsew')
    actions=ttk.Frame(frame);actions.grid(row=9,column=0,columnspan=3,sticky='ew',pady=(16,0))
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
                controller.start(Path(source.get()).expanduser(),Path(parent.get()).expanduser(),ux.default_cache())
            progress.start(12);refresh()
        except (InstallerError,OSError) as exc:status.set(str(exc))
    def folder(path):
        try:open_folder(path)
        except (InstallerError,OSError) as exc:messagebox.showerror('Cannot open folder',str(exc),parent=window)
    build=ttk.Button(actions,text='Run demonstration' if synthetic else 'Build MAG',command=start);build.pack(side='left')
    cancel=ttk.Button(actions,text='Cancel',command=controller.cancel);cancel.pack(side='left',padx=8)
    open_output=ttk.Button(actions,text='Open output',command=lambda:folder(controller.display_output(Path(parent.get()).expanduser())));open_output.pack(side='right')
    open_logs=ttk.Button(actions,text='Open logs',command=lambda:folder(controller.logs));open_logs.pack(side='right',padx=8)
    def refresh():
        for widget in controls:widget.configure(state='disabled' if controller.busy else 'normal')
        build.configure(state='disabled' if controller.busy or controller.blocked else 'normal')
        cancel.configure(state='normal' if controller.busy else 'disabled')
        output=controller.display_output(Path(parent.get()).expanduser())
        destination_label.configure(text=f'New output folder: {output}')
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


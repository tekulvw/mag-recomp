"""Small Tk wizard with a headless, queue-driven transaction controller."""
from __future__ import annotations

from pathlib import Path
import os
import json
import queue
import subprocess
import sys
import threading
import time

from .console import write_line
from .model import InstallerError, Profile
from .safety import Cancellation, Cancelled
from . import ux
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
    def __init__(self, root, readiness=None, installer=None, detector=None, reviewed=None, updater=None):
        self.root = Path(root)
        self._installer = installer
        self._updater = updater
        self._detector, self._reviewed = detector, reviewed
        self.detection = None
        self.guide_events = queue.Queue()
        self.guide_worker = None
        self._guide_request = 0
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

    def survey(self, game_path='', firmware_path='', search=False):
        """Find RPCS3's files (once, or again with search) and check the chosen inputs in the background.

        Read-only: nothing is copied, moved or decrypted. The newest request's result arrives as a
        'guide' event from poll_guide; an older one still running is superseded, not waited for."""
        self._guide_request += 1
        request = self._guide_request
        def run():
            try:
                from . import input_guide, rpcs3_detect
                if search or self.detection is None:
                    self.guide_events.put({'event': 'searching', 'request': request})
                    self.detection = (self._detector or rpcs3_detect.detect)()
                if self._reviewed is None:
                    self._reviewed = input_guide.load_reviewed(self.root) or {}
                steps, game, firmware, game_found, firmware_found = input_guide.survey(
                    self.detection, self._reviewed or None, game_path or None, firmware_path or None)
                self.guide_events.put({'event': 'guide', 'request': request, 'steps': steps, 'game': game,
                                       'firmware': firmware, 'game_path': game_found, 'firmware_path': firmware_found,
                                       'text': input_guide.render(steps, self.detection), 'detection': self.detection})
            except Exception as exc:  # the guide is advice; Build MAG still checks everything
                self.guide_events.put({'event': 'guide-failed', 'request': request,
                                       'message': f'Could not check the files: {exc or type(exc).__name__}'})
        self.guide_worker = threading.Thread(target=run, name='mag-installer-guide', daemon=True)
        self.guide_worker.start()
        return request

    def poll_guide(self):
        """Events of the newest survey request only."""
        events = []
        while True:
            try:
                event = self.guide_events.get_nowait()
            except queue.Empty:
                return events
            if event.get('request') == self._guide_request:
                events.append(event)

    VOICE_HINT = ('Builds in-match voice chat (push to talk, being tested). After the build, turn it on in MAG Settings '
                  '→ General → Voice and hold K to talk. Your choice is remembered for later builds.')

    def update_plan(self, parent, cache):
        """What Update MAG would do for the MAG folder in parent (player_build.update_plan), or None
        when parent holds no complete MAG folder to update."""
        if self.blocked:
            return None
        try:
            from .player_build import update_plan
            return update_plan(self.root, ux.destination(parent), cache)
        except (InstallerError, OSError):
            return None

    @staticmethod
    def update_label(plan):
        """The main button while an existing MAG folder is chosen: "Update MAG (0.15.0 → 0.16.0)"."""
        return f'Update MAG ({plan.get("from_version") or "an earlier MAG"} → {plan.get("to_version") or "this version"})'

    def start(self, source, parent, cache, retry=False, jobs=None, voice=None, firmware=None, update=False):
        """update: update the existing MAG folder in parent in place (player_build.update); source and
        firmware may then be empty, for the remembered ones."""
        if self.blocked:
            raise InstallerError(self.blocked)
        if self.busy:
            raise InstallerError('A build is already running')
        if jobs is not None and str(jobs).strip().lower() != 'auto':
            from .build_jobs import parse
            jobs = parse(jobs)  # a clear error before anything starts
        from .player_build import build_folder
        output = ux.destination(parent).absolute()
        if (output.exists() or output.is_symlink()) and not retry and not update and not self.legacy_install(parent):
            raise InstallerError('Output already exists; choose another installation parent. Existing files are preserved.')
        if not update and not str(source or '').strip():
            raise InstallerError('Choose your game folder and installation location.')
        self.output, self.logs = output, build_folder(output)/'logs'
        self.updating = update
        self.token = Cancellation()
        self.error = ''
        self.state = 'installing'
        def run():
            try:
                self.token.check()
                if update:
                    upgrade = self._updater
                    if upgrade is None:
                        from . import player_build
                        upgrade = player_build.update
                    result = upgrade(self.root, output, Path(cache).expanduser(), token=self.token, events=self.events.put,
                                     source=Path(source).expanduser() if str(source or '').strip() else None,
                                     firmware=Path(firmware).expanduser() if firmware else None,
                                     **({'jobs': jobs} if jobs is not None else {}), **({'voice': voice} if voice is not None else {}))
                    self.events.put({'event':'_complete','output':str(result or output)})
                    return
                build = self._installer
                if build is None:
                    from . import player_build
                    build = player_build.install
                result = build(self.root, Path(source).expanduser(), output, Path(cache).expanduser(), token=self.token, events=self.events.put,
                               **({'retry': True} if retry else {}), **({'jobs': jobs} if jobs is not None else {}),
                               **({'voice': voice} if voice is not None else {}),
                               **({'firmware': Path(firmware).expanduser()} if firmware else {}))
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
        window = tk.Tk()
    except Exception as exc:
        write_line(sys.stderr,f'Cannot open the installer window: {exc}. Use a Python runtime with matching Tcl/Tk support, or --help for CLI commands. Bundled installers must include Tcl/Tk.')
        return 2
    try:
        open_window(window, root, synthetic)
    except (InstallerError,OSError) as exc:
        window.destroy();write_line(sys.stderr,str(exc));return 2
    window.mainloop()
    return 0


def _check_summary(findings, found):
    """One line under a picker: the first check result for that input, in words."""
    if not findings:
        return ''
    first = findings[0]
    word = {'Done': 'Checked', 'To do': 'Not ready yet', 'Problem': 'Problem'}[first.level]
    message = first.message.rstrip(':') + ('.' if first.message.endswith(':') else '')
    return (f'{word}{" (found by the installer)" if found else ""}: {message}'
            + (' The guide above shows the command.' if first.commands else ''))


def open_window(window, root: Path, synthetic: bool = False, controller=None):
    """Lay out the wizard in window (a tk.Tk) and start its polling; returns the widgets tests inspect.

    launch runs its main loop. The player wizard shows the preparation guide (input_guide.py) above
    the pickers: RPCS3's files are looked for in the background, found paths fill empty fields, and
    each chosen input is checked at once."""
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
    if controller is None:
        controller = (WizardController(Profile.load(Path(root)/'profiles/synthetic-demonstration.json'),True)
                      if synthetic else PlayerBuildController(root))
    window.title('MAG Installer' + (' — Synthetic demonstration' if synthetic else ''))
    height=min(900,max(640,window.winfo_screenheight()-80))
    window.geometry(f'860x{height}');window.minsize(680,600)
    frame=ttk.Frame(window,padding=24);frame.pack(fill='both',expand=True)
    frame.columnconfigure(1,weight=1)
    ttk.Label(frame,text='Installer demonstration' if synthetic else 'Build MAG',font=('TkDefaultFont',22,'bold')).grid(row=0,column=0,columnspan=3,sticky='w',pady=(0,12))
    intro=('SYNTHETIC ONLY. Uses sample text; no game is built or playable.' if synthetic else 'Choose your game files and installation location. Build MAG handles the required tools and build steps automatically.')
    ttk.Label(frame,text=intro,wraplength=760).grid(row=1,column=0,columnspan=3,sticky='w',pady=(0,12))
    source=tk.StringVar();parent=tk.StringVar(value=str(ux.default_parent()));firmware=tk.StringVar()
    controls=[]
    guide_text=guide_status=None
    checks={}
    if synthetic:
        notice=controller.blocked or 'Input needs SYNTHETIC-INPUT.txt containing “This is synthetic installer input.” followed by one LF newline.'
        ttk.Label(frame,text=notice,wraplength=760).grid(row=2,column=0,columnspan=3,sticky='w',pady=(0,12))
    else:
        # The preparation guide: a read-only, selectable text box (commands can be copied) and its actions.
        guide=ttk.LabelFrame(frame,text='Getting your files ready',padding=8)
        guide.grid(row=2,column=0,columnspan=3,sticky='nsew',pady=(0,8));guide.columnconfigure(0,weight=1);guide.rowconfigure(1,weight=1)
        guide_status=tk.StringVar(value='Looking for RPCS3 and your MAG files…')
        ttk.Label(guide,textvariable=guide_status,wraplength=760).grid(row=0,column=0,columnspan=2,sticky='w')
        guide_text=tk.Text(guide,height=11,wrap='word',font='TkFixedFont',relief='flat',takefocus=True)
        guide_text.grid(row=1,column=0,sticky='nsew',pady=(4,4))
        scroll=ttk.Scrollbar(guide,orient='vertical',command=guide_text.yview);scroll.grid(row=1,column=1,sticky='ns',pady=(4,4))
        guide_text.configure(yscrollcommand=scroll.set,state='disabled')
        guide_text.bind('<1>',lambda event:guide_text.focus_set())
        guide_actions=ttk.Frame(guide);guide_actions.grid(row=2,column=0,columnspan=2,sticky='w')
        from .input_guide import RPCS3_URL, FIRMWARE_PUP_URL, COMMAND_INDENT
        def visit(url):
            import webbrowser
            try:webbrowser.open(url)
            except Exception:pass
            guide_status.set(f'Opened {url} in your browser.')
        def copy_commands():
            lines=[line.strip() for line in guide_text.get('1.0','end').splitlines() if line.startswith(COMMAND_INDENT)]
            if not lines:
                guide_status.set('No commands to copy: nothing is waiting for RPCS3.');return
            window.clipboard_clear();window.clipboard_append('\n'.join(lines))
            guide_status.set(f'Copied {len(lines)} command{"s" if len(lines)>1 else ""}. Paste {"it" if len(lines)==1 else "them"} into a terminal.')
        ttk.Button(guide_actions,text='RPCS3 download page',command=lambda:visit(RPCS3_URL)).pack(side='left')
        ttk.Button(guide_actions,text='Sony system software page',command=lambda:visit(FIRMWARE_PUP_URL)).pack(side='left',padx=8)
        ttk.Button(guide_actions,text='Copy commands',command=copy_commands).pack(side='left')
        ttk.Button(guide_actions,text='Look again',command=lambda:survey(search=True)).pack(side='left',padx=8)
    rows=((3,'Game folder' if not synthetic else 'Sample input folder',source,'folder'),
          *(() if synthetic else ((5,'Firmware file',firmware,'file'),)),
          (7,'Install inside',parent,'folder'))
    def browse(variable,kind):
        if kind=='file':
            current=variable.get().strip()
            chosen=filedialog.askopenfilename(parent=window,title='Choose the decrypted firmware module (libsre.prx)',
                                              initialdir=str(Path(current).parent) if current else str(Path.home()),
                                              filetypes=[('Decrypted firmware module','*.prx *.elf'),('All files','*')])
        else:
            chosen=filedialog.askdirectory(parent=window,initialdir=variable.get() or str(Path.home()))
        if chosen:
            variable.set(chosen)
            if variable is not parent and not synthetic:survey()
    for row,label,variable,kind in rows:
        ttk.Label(frame,text=label).grid(row=row,column=0,sticky='w',padx=(0,12),pady=(6,0))
        entry=ttk.Entry(frame,textvariable=variable);entry.grid(row=row,column=1,sticky='ew',pady=(6,0))
        button=ttk.Button(frame,text='Browse…',command=lambda v=variable,k=kind:browse(v,k));button.grid(row=row,column=2,padx=(8,0),pady=(6,0))
        controls.extend((entry,button))
        if variable is not parent and not synthetic:
            # The check result for this input, in words; Return or leaving the field checks a typed path.
            checks[label]=tk.StringVar(value='')
            ttk.Label(frame,textvariable=checks[label],wraplength=600).grid(row=row+1,column=1,columnspan=2,sticky='w')
            entry.bind('<Return>',lambda event:survey());entry.bind('<FocusOut>',lambda event:survey(changed_only=True))
    jobs=jobs_choice=voice=voice_initial=None
    if not synthetic:
        # Build option: how many compile jobs run at once (build_jobs.py), remembered between builds.
        jobs_choice=controller.jobs_choice(ux.default_cache())
        jobs=tk.StringVar(value=str(jobs_choice['value']))
        ttk.Label(frame,text='Compile jobs').grid(row=8,column=0,sticky='nw',padx=(0,12),pady=6)
        jobs_row=ttk.Frame(frame);jobs_row.grid(row=8,column=1,columnspan=2,sticky='ew',pady=6);jobs_row.columnconfigure(1,weight=1)
        spin=ttk.Spinbox(jobs_row,from_=jobs_choice['minimum'],to=jobs_choice['maximum'],increment=1,width=5,textvariable=jobs)
        spin.grid(row=0,column=0,sticky='nw')
        ttk.Label(jobs_row,text=controller.jobs_hint(jobs_choice),wraplength=600).grid(row=0,column=1,sticky='w',padx=(12,0))
        controls.append(spin)
        # Build option: in-match voice chat (voice_option.py), off by default, remembered between builds.
        voice_initial=controller.voice_choice(ux.default_cache())
        voice=tk.BooleanVar(value=voice_initial)
        ttk.Label(frame,text='Voice chat').grid(row=9,column=0,sticky='nw',padx=(0,12),pady=6)
        voice_row=ttk.Frame(frame);voice_row.grid(row=9,column=1,columnspan=2,sticky='ew',pady=6);voice_row.columnconfigure(1,weight=1)
        voice_box=ttk.Checkbutton(voice_row,text='Include',variable=voice)
        voice_box.grid(row=0,column=0,sticky='nw')
        ttk.Label(voice_row,text=controller.VOICE_HINT,wraplength=600).grid(row=0,column=1,sticky='w',padx=(12,0))
        controls.append(voice_box)
    destination_label=ttk.Label(frame,wraplength=760);destination_label.grid(row=10,column=0,columnspan=3,sticky='w',pady=8)
    status=tk.StringVar(value=controller.blocked or 'Ready when you are.')
    ttk.Label(frame,textvariable=status,wraplength=760).grid(row=11,column=0,columnspan=3,sticky='w',pady=8)
    progress=ttk.Progressbar(frame,mode='indeterminate');progress.grid(row=12,column=0,columnspan=3,sticky='ew',pady=(0,10))
    details=tk.Text(frame,height=9 if synthetic else 5,wrap='word',state='disabled');details.grid(row=13,column=0,columnspan=3,sticky='nsew')
    frame.rowconfigure(13,weight=1)
    if not synthetic:frame.rowconfigure(2,weight=2)
    actions=ttk.Frame(frame);actions.grid(row=14,column=0,columnspan=3,sticky='ew',pady=(16,0))
    closing=False
    surveyed={'source':None,'firmware':None}
    filled={'source':'','firmware':''}
    def survey(search=False,changed_only=False):
        if synthetic:return
        current=(source.get().strip(),firmware.get().strip())
        if changed_only and (surveyed['source'],surveyed['firmware'])==current:return
        surveyed['source'],surveyed['firmware']=current
        for variable in checks.values():variable.set('Checking…' if any(current) else '')
        guide_status.set('Looking for RPCS3 and your MAG files…' if search or controller.detection is None else 'Checking your files…')
        controller.survey(*current,search=search)
    def show_guide(event):
        from .input_guide import DONE
        guide_text.configure(state='normal');guide_text.delete('1.0','end');guide_text.insert('1.0',event['text'])
        guide_text.configure(state='disabled')
        detection=event['detection']
        found=[]
        # Found paths fill only empty fields, and only once: a choice of the player's own is never replaced.
        if event['game_path'] and not source.get().strip() and not filled['source']:
            filled['source']=str(event['game_path']);source.set(filled['source']);found.append('your MAG game folder')
        if event['firmware_path'] and not firmware.get().strip() and not filled['firmware']:
            filled['firmware']=str(event['firmware_path']);firmware.set(filled['firmware']);found.append('the firmware module')
        surveyed['source'],surveyed['firmware']=source.get().strip(),firmware.get().strip()
        game=event['game']
        checks['Game folder'].set(_check_summary(game.findings if game else [], bool(filled['source']) and source.get().strip()==filled['source']))
        checks['Firmware file'].set(_check_summary(event['firmware'] or [], bool(filled['firmware']) and firmware.get().strip()==filled['firmware']))
        ready=all(step.state==DONE for step in event['steps'])
        where=('RPCS3 found' if detection.rpcs3_found() else 'RPCS3 not found')+('; the search stopped at its time limit' if detection.incomplete else '')
        guide_status.set((f'Filled in {" and ".join(found)}. ' if found else '')+('All files are ready: click Build MAG.' if ready else
                         f'{where}. Follow the steps marked "To do" or "Problem", then choose the files below.'))
    def append(text):
        details.configure(state='normal');details.insert('end',text+'\n');details.see('end');details.configure(state='disabled')
    plan_cache={'key':None,'time':0.0,'plan':None,'shown':None}
    def update_plan():
        """The Update MAG plan for the chosen location (rechecked at most every 2 s), or None."""
        if synthetic:return None
        key=parent.get().strip()
        if not key:return None
        now=time.monotonic()
        if plan_cache['key']!=key or now-plan_cache['time']>2:
            plan_cache.update(key=key,time=now,plan=controller.update_plan(Path(key).expanduser(),ux.default_cache()))
        return plan_cache['plan']
    def show_plan(plan):
        """When a MAG folder to update is chosen: its remembered inputs fill empty fields, and the voice
        chat box shows the option it was built with."""
        nonlocal voice_initial
        output=str(plan['output']) if plan else None
        if plan_cache['shown']==output:return
        plan_cache['shown']=output
        if not plan:return
        remembered=plan.get('remembered') or {}
        filled=False
        if not source.get().strip() and remembered.get('game_folder') and Path(remembered['game_folder']).is_dir():
            source.set(remembered['game_folder']);filled=True
        if not firmware.get().strip() and remembered.get('firmware') and Path(remembered['firmware']).is_file():
            firmware.set(remembered['firmware']);filled=True
        options=plan.get('options') or {}
        if voice is not None and isinstance(options.get('voice'),bool):
            voice.set(options['voice']);voice_initial=options['voice']
        if filled:survey()
    def start():
        try:
            plan=update_plan()
            if plan:
                controller.start(source.get().strip(),Path(parent.get()).expanduser(),ux.default_cache(),update=True,
                                 jobs=controller.jobs_request(jobs.get(),jobs_choice['default']),
                                 voice=(voice.get() if voice.get()!=voice_initial else None),
                                 firmware=firmware.get().strip() or None)
                progress.start(12);refresh();return
            if not source.get().strip() or not parent.get().strip():raise InstallerError('Choose your game folder and installation location.')
            if synthetic:
                resume=controller.has_journal(Path(parent.get()).expanduser())
                if resume and not messagebox.askyesno('Retry demonstration?', 'Interrupted demonstration work exists. Reverify input and replay its stages?',parent=window):return
                controller.start(Path(source.get()).expanduser(),Path(parent.get()).expanduser(),ux.default_cache(),install=True,resume=resume)
            else:
                controller.start(Path(source.get()).expanduser(),Path(parent.get()).expanduser(),ux.default_cache(),retry=controller.has_journal(Path(parent.get()).expanduser()),
                                 jobs=controller.jobs_request(jobs.get(),jobs_choice['default']),
                                 voice=(voice.get() if voice.get()!=voice_initial else None),
                                 firmware=firmware.get().strip() or None)
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
        plan=None
        if not synthetic:
            plan=update_plan() if not controller.busy else None
            if not controller.busy:show_plan(plan)
            build.configure(text=controller.update_label(plan) if plan else
                            ('Retry build' if controller.has_journal(Path(parent.get()).expanduser()) else 'Build MAG'))
            legacy=not controller.busy and not plan and controller.legacy_install(Path(parent.get()).expanduser())
        cancel.configure(state='normal' if controller.busy else 'disabled')
        if clear_cache is not None:clear_cache.configure(state='disabled' if controller.busy else 'normal')
        output=controller.display_output(Path(parent.get()).expanduser())
        destination_label.configure(text=(f'{output} holds MAG. {controller.update_label(plan)} builds the new version in {output}{BUILD_SUFFIX}, '
                                           'then replaces only the app: the folder, its paths, your saves, settings and logs stay, and the '
                                           f'previous version is kept in {output}{BUILD_SUFFIX}{os.sep}previous. Leave the game folder and '
                                           'firmware file empty to use the ones from last time.'
                                           if plan else
                                           f'{output} was built by an earlier installer. Build MAG moves its build files beside it, to {output}{BUILD_SUFFIX}, without rebuilding.'
                                           if legacy else f'New output folder: {output}' + ('' if synthetic else f' (build files: {output}{BUILD_SUFFIX})')))
        open_output.configure(state='normal' if output.is_dir() else 'disabled')
        open_logs.configure(state='normal' if controller.logs and controller.logs.is_dir() else 'disabled')
    def tick():
        for event in controller.poll():
            name=event.get('event','')
            if name=='_complete':
                status.set('Synthetic demonstration complete. No MAG application was built.' if synthetic else
                           'Update finished. Start MAG as before; your saves, settings and logs were kept.' if getattr(controller,'updating',False) else
                           'Build finished. Open output to inspect the application. No game was launched.')
                append(status.get()+f'\nOutput: {controller.output}\nLogs: {controller.logs}')
            elif name in ('_failed','_cancelled'):
                status.set(controller.error);append(controller.error+f'\nLogs: {controller.logs}')
            elif visible_progress_event(event):
                status.set(ux.format_event(event));append(status.get())
        if not synthetic:
            for event in controller.poll_guide():
                if event['event']=='guide':show_guide(event)
                elif event['event']=='guide-failed':guide_status.set(event['message'])
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
    refresh();window.after(100,tick)
    if not synthetic:survey()
    return {'controller':controller,'source':source,'firmware':firmware,'parent':parent,'guide':guide_text,
            'guide_status':guide_status,'checks':checks,'survey':survey,'build':build}


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


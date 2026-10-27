import contextlib
import io
import json
from pathlib import Path
import types
import unittest
from unittest.mock import patch
from mag_installer.__main__ import main

class NativeCliTests(unittest.TestCase):
    def test_native_preflight_explicit_local_paths(self):
        calls=[]
        module=types.SimpleNamespace(preflight=lambda config,output,token=None:(calls.append((config,output)) or {'experimental':True}))
        stream=io.StringIO()
        with patch.dict('sys.modules',{'mag_installer.native_build':module}),patch('mag_installer.native_build',module,create=True),contextlib.redirect_stdout(stream):
            self.assertEqual(main(['native-preflight','--config','/local/recipe.json','--output','/new/build','--json']),0)
        self.assertEqual(calls,[(Path('/local/recipe.json'),Path('/new/build'))])
        self.assertTrue(json.loads(stream.getvalue())['experimental'])

    def test_native_build_uses_cancellation_and_never_resumes(self):
        calls=[]
        def install(config,output,**kwargs):calls.append(kwargs);return output
        module=types.SimpleNamespace(install=install)
        with patch.dict('sys.modules',{'mag_installer.native_build':module}),patch('mag_installer.native_build',module,create=True),contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(main(['native-build','--config','/recipe.json','--output','/new/build']),0)
        self.assertFalse(calls[0]['resume'])
        self.assertIsNotNone(calls[0]['token'])

    def test_wizard_has_no_local_recipe_option(self):
        with contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit):
            main(['wizard','--local-build','/trusted/recipe.json'])

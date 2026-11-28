import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class EngineHookTests(unittest.TestCase):
    def test_original_setter_and_no_ticket_validation_preserve_guest_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp/'ppu_recomp.h').write_text('''#pragma once
#include <cstdint>
struct ppu_context { uint64_t gpr[32]{}; uint64_t lr{}; };
extern "C" {
extern uint8_t* vm_base;
extern uint32_t ppu_vm_size;
uint8_t vm_read8(uint64_t);
uint32_t vm_read32(uint64_t);
void vm_write8(uint64_t,uint8_t);
void vm_write32(uint64_t,uint32_t);
void ppu_register_function(uint64_t,void(*)(ppu_context*));
}
''')
            binary = tmp/'check'
            subprocess.run(['c++', '-std=c++17', '-I'+str(tmp), '-I'+str(ROOT/'src'),
                str(ROOT/'src/v212_engine_hooks.cpp'),
                str(ROOT/'tests/v212_engine_hooks_fixture.cpp'), '-o', str(binary)], check=True)
            accepted = ('disabled', 'valid', 'repeat', 'configured-name', 'unhandled',
                        'settings-conflict-only', 'settings-ticket-only', 'trace-only')
            rejected = ('ticket-file', 'ticket-config', 'identity-conflict', 'locked',
                        'guard', 'opd-guard', 'connect-guard', 'connect-opd-guard',
                        'connect-vtable-guard', 'out-of-bounds', 'invalid-name', 'setter-failure',
                        'account-without-local', 'implicit-local', 'bad-stack', 'capacity',
                        'repeat-locked', 'repeat-length', 'repeat-capacity', 'repeat-identity')
            for mode in accepted + rejected:
                result = subprocess.run([str(binary), mode], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0 if mode in accepted else 89,
                                 mode+': '+result.stderr)
                self.assertNotIn('FixtureUser', result.stderr)
                if mode == 'unhandled':
                    self.assertNotIn('settings ticket=', result.stderr)
                if mode in ('locked','capacity','identity-conflict','repeat-locked',
                            'repeat-length','repeat-capacity','repeat-identity'):
                    self.assertIn('account policy rejected cfg=0x00003028', result.stderr)
                    self.assertIn('selected_length=11', result.stderr)
                    self.assertIn('ticket=0', result.stderr)
                    self.assertIn('getter=0x3863004C caller_lr=0xABC00004', result.stderr)
                if mode == 'repeat-locked':
                    self.assertIn('length=11 capacity=15 selected_length=11 locked=1 ticket=0 matches=1', result.stderr)
                if mode == 'repeat-capacity':
                    self.assertIn('length=11 capacity=31', result.stderr)
                if mode == 'repeat-length':
                    self.assertIn('length=4 capacity=15', result.stderr)
                if mode == 'repeat-identity':
                    self.assertIn('matches=0', result.stderr)

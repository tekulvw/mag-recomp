"""The retail override must remain opt-in and reject mismatched code/config."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class RetailNetworkPolicyTests(unittest.TestCase):
    def test_configuration_and_instruction_guards(self):
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp)/'test.cpp'; binary=Path(tmp)/'test'
            source.write_text('''#include "retail_network_policy.h"
#include <cassert>
#include <initializer_list>
int main() {
    assert(mag_local_account_policy(false,0,1,1,9,99,6,false)==0);
    assert(mag_local_account_policy(true,0,0,0,0,15,6,false)==-1);
    assert(mag_local_account_policy(true,0x3863004c,1,0,0,15,6,false)==-1);
    assert(mag_local_account_policy(true,0x3863004c,0,1,0,15,6,false)==-1);
    assert(mag_local_account_policy(true,0x3863004c,0,0,0,31,6,false)==-1);
    assert(mag_local_account_policy(true,0x3863004c,0,0,5,15,6,false)==-1);
    assert(mag_local_account_policy(true,0x3863004c,0,0,6,15,6,true)==0);
    assert(mag_local_account_policy(true,0x3863004c,0,0,12,15,12,true)==0);
    assert(mag_local_account_policy(true,0x3863004c,0,0,12,15,12,false)==-1);
    assert(mag_local_account_policy(true,0x3863004c,0,0,16,15,16,true)==-1);
    assert(mag_local_account_policy(true,0x3863004c,0,0,0,15,6,false)==1);
    // Any valid name, not only the two development identities.
    assert(mag_local_account_policy(true,0x3863004c,0,0,0,15,9,false)==1);
    assert(mag_local_account_policy(true,0x3863004c,0,0,9,15,9,true)==0);
    assert(mag_local_account_policy(true,0x3863004c,0,0,6,15,9,true)==-1);  // a different name is never replaced
    assert(mag_local_account_policy(true,0x3863004c,0,0,0,15,2,false)==-1);
    assert(mag_local_account_policy(true,0x3863004c,0,0,0,15,16,false)==-1);
    for (const char* good : {"Player", "PlayerRetail", "Rook_7", "abc", "Pilot-One-15chr"}) assert(mag_local_account_name_valid(good));
    for (const char* bad : {"", "ab", "7up", "_abc", "bad name", "sixteen-chars-xx", "n\xc3\xa4me", (const char*)nullptr})
        assert(!mag_local_account_name_valid(bad));
    unsigned char effective=1;
    assert(mag_restore_local_ticket(false,0,0,0,effective)==0 && effective==1);
    assert(mag_restore_local_ticket(true,0,0x98092280,0,effective)==-1 && effective==1);
    assert(mag_restore_local_ticket(true,0x38000001,0,0,effective)==-1 && effective==1);
    assert(mag_restore_local_ticket(true,0x38000001,0x98092280,1,effective)==-1 && effective==1);
    assert(mag_restore_local_ticket(true,0x38000001,0x98092280,0,effective)==1 && effective==0);
    assert(mag_restore_local_ticket(true,0x38000001,0x98092280,0,effective)==0 && effective==0);
}
''')
            subprocess.run(['c++','-std=c++17','-UNDEBUG','-I'+str(ROOT/'src'),str(source),'-o',str(binary)],check=True,timeout=30)
            subprocess.run([str(binary)],check=True,timeout=5)

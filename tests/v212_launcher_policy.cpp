#include "v212_launcher_hooks.h"
#include <cassert>
int main() {
    uint8_t effective=1;
    assert(mag_v212_restore_local_ticket(false,0,0,1,effective)==0 && effective==1);
    assert(mag_v212_restore_local_ticket(true,0x38000001,0x98092289,0,effective)==1 && effective==0);
    assert(mag_v212_restore_local_ticket(true,0x38000001,0x98092289,0,effective)==0);
    effective=1;
    assert(mag_v212_restore_local_ticket(true,0x38000001,0x98092280,0,effective)==-1 && effective==1);
    assert(mag_v212_restore_local_ticket(true,0x38000001,0x98092289,1,effective)==-1 && effective==1);
    assert(mag_local_account_policy(true,0x3863004c,0,0,0,15,10,false)==1);
    assert(mag_local_account_policy(true,0x3863004c,0,0,4,15,10,false)==-1);
    assert(mag_local_account_policy(true,0x3863004c,0,1,0,15,10,false)==-1);
    assert(mag_local_account_policy(true,0x3863004c,1,0,0,15,10,false)==-1);
}

#include "retail_network_policy.h"
#include <cassert>
#include <string>
int main() {
  for (const char* name : {"abc", "Pilot-7", "PlayerRetail", "ABCDEFGHIJKLMNO"})
    assert(mag_local_account_name_valid(name));
  for (const char* name : {"", "ab", "abcdefghijklmnop", "bad name", "a/b", "a.b", "caf\xc3\xa9"})
    assert(!mag_local_account_name_valid(name));
  assert(!mag_local_account_name_valid(nullptr));
  for (unsigned length : {3u, 7u, 15u}) {
    assert(mag_local_account_policy(true,0x3863004c,0,0,length,15,true)==0);
    assert(mag_local_account_policy(true,0x3863004c,0,0,length,15,false)==-1);
  }
  assert(mag_local_account_policy(true,0x3863004c,0,0,0,15,false)==1);
  assert(mag_local_account_policy(true,0x3863004c,1,0,0,15,false)==-1);
  assert(mag_local_account_policy(true,0x3863004c,0,1,0,15,false)==-1);
  assert(mag_local_account_policy(true,0x3863004c,0,0,16,15,true)==-1);
  assert(mag_local_account_policy(true,0,0,0,0,15,false)==-1);
  assert(mag_local_account_policy(false,0,1,1,9,99,false)==0);
  assert(mag_local_account_policy(true,0x3863004c,0,0,0,31,false)==-1);
  unsigned char effective=1;
  assert(mag_restore_local_ticket(false,0,0,0,effective)==0 && effective==1);
  assert(mag_restore_local_ticket(true,0,0x98092280,0,effective)==-1 && effective==1);
  assert(mag_restore_local_ticket(true,0x38000001,0,0,effective)==-1 && effective==1);
  assert(mag_restore_local_ticket(true,0x38000001,0x98092280,1,effective)==-1 && effective==1);
  assert(mag_restore_local_ticket(true,0x38000001,0x98092280,0,effective)==1 && effective==0);
  assert(mag_restore_local_ticket(true,0x38000001,0x98092280,0,effective)==0 && effective==0);
}

#include "../src/mag_payload_clock_observation.h"
#include <cassert>
#include <initializer_list>
int main(){
 assert(mag_payload_clock_entry(nullptr)==0x7848);
 assert(mag_payload_clock_entry("13100")==0x13100);
 assert(mag_payload_clock_entry("0x7848")==0x7848);
 assert(mag_payload_clock_entry("0X3FFFC")==0x3fffc);
 for(const char*s:{"","0","-1","+7848"," 7848","7848x","40000","13101","100000000","0x"})assert(mag_payload_clock_entry(s)==0);
 MagPayloadClockObservation begin{10,50,1,true,true},end{20,70,1,true,true};
 uint64_t delta=999;
 assert(mag_payload_clock_delta(begin,end,&delta)&&delta==20);
 end.host=2;assert(!mag_payload_clock_delta(begin,end,&delta)&&delta==0);
 end.host=1;end.cpu=40;assert(!mag_payload_clock_delta(begin,end,&delta));
 end.cpu=70;end.cpu_valid=false;assert(!mag_payload_clock_delta(begin,end,&delta));
 end.cpu_valid=true;end.host_valid=false;assert(!mag_payload_clock_delta(begin,end,&delta));
 end.host_valid=true;end.wall=9;assert(!mag_payload_clock_delta(begin,end,&delta));
 end.wall=20;begin.host=0;assert(!mag_payload_clock_delta(begin,end,&delta));
 assert(!mag_payload_clock_delta(begin,end,nullptr));
}

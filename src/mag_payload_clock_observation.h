#pragma once
#include <stdint.h>
/* Host CPU over a payload observation interval is meaningful only when both
 * clocks succeeded and the invocation returns on the same host thread. It is
 * not exclusive payload CPU if other virtual contexts ran in between. */
struct MagPayloadClockObservation {
 uint64_t wall,cpu,host;
 bool cpu_valid,host_valid;
};
inline bool mag_payload_clock_delta(const MagPayloadClockObservation&a,const MagPayloadClockObservation&b,uint64_t*out){
 if(!out)return false;
 *out=0;
 if(!a.cpu_valid||!b.cpu_valid||!a.host_valid||!b.host_valid||!a.host||a.host!=b.host||b.wall<a.wall||b.cpu<a.cpu)return false;
 *out=b.cpu-a.cpu;return true;
}

/* A cached launch selector; invalid values disable selection, rather than
 * broadening it. No parsing occurs in the payload or instruction hot path. */
inline uint32_t mag_payload_clock_entry(const char *text){
 if(!text)return 0x7848;
 if(text[0]=='0'&&(text[1]=='x'||text[1]=='X'))text+=2;
 uint32_t value=0;unsigned digits=0;
 for(;*text;++text){
  const unsigned char c=(unsigned char)*text;unsigned digit;
  if(c>='0'&&c<='9')digit=c-'0';
  else if(c>='a'&&c<='f')digit=c-'a'+10;
  else if(c>='A'&&c<='F')digit=c-'A'+10;
  else return 0;
  if(++digits>5)return 0;
  value=(value<<4)|digit;
 }
 return digits&&value&&value<0x40000&&!(value&3)?value:0;
}

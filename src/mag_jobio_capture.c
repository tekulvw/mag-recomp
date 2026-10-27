/* MAG-only observation adapter around the selected SDK's recorder. The SDK
 * source and guest DMA/atomic behavior are unchanged. Default recording remains
 * the original complete v1 stream. Opt-in transfer-only v2 omits atomic input,
 * output and status observations, so it is never a full job replay fixture. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "diagnostics/job_io_recorder.h"
static int mag_transfer_only;
static _Thread_local int mag_header_pending;
static size_t mag_jobio_write(const void *ptr,size_t size,size_t count,FILE *file){
 if(!mag_header_pending)return fwrite(ptr,size,count,file);
 mag_header_pending=0;
 /* The SDK writes this header first inside finalization. Refuse any changed
  * format instead of accidentally exposing filtered records as complete v1. */
 if(size!=8*sizeof(uint32_t)||count!=1)return 0;
 uint32_t header[8];memcpy(header,ptr,sizeof header);
 if(header[0]!=0x4a4f4249u||header[1]!=1)return 0;
 header[1]=2;return fwrite(header,sizeof header,1,file);
}
#define ps3_jobio_begin mag_jobio_sdk_begin
#define ps3_jobio_event mag_jobio_sdk_event
#define ps3_jobio_end mag_jobio_sdk_end
#define fwrite mag_jobio_write
#include "diagnostics/job_io_recorder.c"
#undef fwrite
#undef ps3_jobio_begin
#undef ps3_jobio_event
#undef ps3_jobio_end

static pthread_once_t mag_mode_once=PTHREAD_ONCE_INIT;
static pthread_mutex_t mag_filter_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t mag_raw[7],mag_kept[7],mag_skipped[7];
static void mag_mode_init(void){const char* flag=getenv("MAG_JOB_IO_TRANSFERS_ONLY");mag_transfer_only=flag&&!strcmp(flag,"1");}
int ps3_jobio_begin(struct spu_context* c,uint32_t tag,const char* directory){
 pthread_once(&mag_mode_once,mag_mode_init);
 if(!mag_transfer_only)return mag_jobio_sdk_begin(c,tag,directory);
 pthread_mutex_lock(&mag_filter_lock);
 const int result=mag_jobio_sdk_begin(c,tag,directory);
 if(!result){memset(mag_raw,0,sizeof mag_raw);memset(mag_kept,0,sizeof mag_kept);memset(mag_skipped,0,sizeof mag_skipped);}
 pthread_mutex_unlock(&mag_filter_lock);return result;
}
void ps3_jobio_event(struct spu_context* c,uint32_t kind,uint32_t cmd,uint32_t lsa,uint64_t ea,uint32_t size,const uint8_t* bytes,uint32_t extra){
 if(!mag_transfer_only){mag_jobio_sdk_event(c,kind,cmd,lsa,ea,size,bytes,extra);return;}
 pthread_mutex_lock(&mag_filter_lock);
 if(ps3_jobio_is(c)){
  if(kind<7)++mag_raw[kind];
  if(kind==PS3_JOBIO_GETLLAR||kind==PS3_JOBIO_PUTLLC||kind==PS3_JOBIO_PUTLLUC){if(kind<7)++mag_skipped[kind];}
  else{mag_jobio_sdk_event(c,kind,cmd,lsa,ea,size,bytes,extra);if(kind<7)++mag_kept[kind];}
 }
 pthread_mutex_unlock(&mag_filter_lock);
}
static int mag_write_mode(unsigned index){
 char recording[PATH_MAX+64],path[PATH_MAX+80],temporary[PATH_MAX+96];
 snprintf(recording,sizeof recording,"%s/job-%04u.jobio",run_directory,index);
 uint32_t header[8];FILE* input=fopen(recording,"rb");
 if(!input)return -1;
 const int read_ok=fread(header,sizeof header,1,input)==1;fclose(input);
 if(!read_ok||header[0]!=0x4a4f4249u||header[1]!=2)return -1;
 snprintf(path,sizeof path,"%s/job-%04u.mode.json",run_directory,index);snprintf(temporary,sizeof temporary,"%s.tmp",path);
 FILE* output=fopen(temporary,"wb");if(!output)return -1;
 int ok=fprintf(output,"{\"format_version\":2,\"mode\":\"transfers-only\",\"full_job_replay\":false,\"atomic_inputs_outputs_and_status_omitted\":true,\"truncated\":%s,\"raw_by_kind\":[",header[6]?"true":"false")>0;
 for(unsigned n=0;n<7;n++)if(fprintf(output,"%s%llu",n?",":"",(unsigned long long)mag_raw[n])<0)ok=0;
 if(fprintf(output,"],\"recorded_by_kind\":[")<0)ok=0;
 for(unsigned n=0;n<7;n++)if(fprintf(output,"%s%llu",n?",":"",(unsigned long long)mag_kept[n])<0)ok=0;
 if(fprintf(output,"],\"skipped_by_kind\":[")<0)ok=0;
 for(unsigned n=0;n<7;n++)if(fprintf(output,"%s%llu",n?",":"",(unsigned long long)mag_skipped[n])<0)ok=0;
 if(fprintf(output,"]}\n")<0)ok=0;
 if(fclose(output))ok=0;
 if(!ok||rename(temporary,path)){remove(temporary);return -1;}
 return header[6]?-1:0;
}
int ps3_jobio_end(struct spu_context* c){
 if(!mag_transfer_only)return mag_jobio_sdk_end(c);
 pthread_mutex_lock(&mag_filter_lock);
 if(!ps3_jobio_is(c)){pthread_mutex_unlock(&mag_filter_lock);return -1;}
 const unsigned index=sequence;mag_header_pending=1;
 int result=mag_jobio_sdk_end(c);mag_header_pending=0;
 if(!result)result=mag_write_mode(index);
 fprintf(stderr,"[mag-jobio-capture] sequence=%u mode=transfers-only version=2 finalized=%u full_job_replay=0 atomic_observations_omitted=1\n",index,result==0);
 pthread_mutex_unlock(&mag_filter_lock);return result;
}

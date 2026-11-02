#import <Foundation/Foundation.h>
#include "mag_render_native_api.h"
#include <cstdio>
#include <cstring>
extern "C" int mag_render_test_owner_open(MagRenderHostOwner *);
extern "C" int mag_render_test_owner_contract(void);
extern "C" int mag_render_test_owner_gate(void);
extern "C" const char *mag_render_test_owner_name(void);
extern "C" void mag_render_test_owner_signal(void *);
extern "C" void mag_render_test_owner_close(int);
int mag_render_draw_main(const char *,bool);
int main(int argc,char **argv) {
    if(argc==3&&!std::strcmp(argv[1],"--draw"))return mag_render_draw_main(argv[2],true);
    if(argc==3&&!std::strcmp(argv[1],"--draw-no-readback"))return mag_render_draw_main(argv[2],false);
    if (argc==2 && !std::strcmp(argv[1],"--owner-contract")) return mag_render_test_owner_contract();
    if (argc!=2) {std::fprintf(stderr,"provide fresh generated copy_transform directory\n");return 2;}
    @autoreleasepool {
        NSString *root=[NSString stringWithUTF8String:argv[1]];
        NSData *source=[NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:@"shader.metal"]];
        NSData *metadata=[NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:@"manifest.json"]];
        if (!source || !metadata) return 2;
        NSDictionary *manifest=[NSJSONSerialization JSONObjectWithData:metadata options:0 error:NULL];
        if ([manifest[@"schema_version"] unsignedIntValue]!=1) return 2;
        NSArray *buffers=manifest[@"buffers"];if (buffers.count!=3) return 2;
        mag_native_binding_layout layouts[16]={};
        for (NSUInteger i=0;i<buffers.count;++i) {
            NSDictionary *entry=buffers[i];uint32_t index=MAG_NATIVE_NO_SIZE_INDEX;
            for (NSDictionary *sizes in manifest[@"metal_sizes_buffer"][@"entries"])
                if ([sizes[@"binding"] isEqual:entry[@"binding"]]) index=[sizes[@"offset_bytes"] unsignedIntValue]/4;
            layouts[i]={[entry[@"binding"] unsignedIntValue],
                [entry[@"access"] isEqual:@"read"]?MAG_NATIVE_READ:MAG_NATIVE_READ_WRITE,
                [entry[@"minimum_binding_bytes"] unsignedLongLongValue],
                index==MAG_NATIVE_NO_SIZE_INDEX?0:[entry[@"layout"][@"stride_bytes"] unsignedIntValue],index,
                [entry[@"layout"][@"alignment_bytes"] unsignedIntValue],
                [entry[@"address_space"] isEqual:@"uniform"]?MAG_NATIVE_UNIFORM:MAG_NATIVE_STORAGE};
        }
        NSString *entry=manifest[@"targets"][@"msl"][@"entry_point"];
        mag_native_pipeline_desc pipeline={};pipeline.struct_size=sizeof(pipeline);pipeline.abi_version=1;
        pipeline.source_bytes=source.bytes;pipeline.source_length=source.length;
        pipeline.entry_point=entry.UTF8String;pipeline.entry_point_length=uint32_t(std::strlen(entry.UTF8String));
        pipeline.binding_count=uint32_t(buffers.count);pipeline.bindings=layouts;pipeline.shader_language=MAG_NATIVE_MSL;
        for (unsigned i=0;i<3;++i) pipeline.threads_per_group[i]=[manifest[@"workgroup_size"][i] unsignedIntValue];
        MagRenderHostOwner owner={};
        if(mag_render_test_owner_open(&owner)!=0 || mag_render_test_owner_gate()!=0) {mag_render_test_owner_close(1);return 3;}
        NSString *deviceName=[NSString stringWithUTF8String:mag_render_test_owner_name()];
        MagRenderNativeProbeReport report={};report.abi_version=1;report.struct_size=sizeof(report);
        int status=mag_render_native_probe_owner(&owner,&pipeline,mag_render_test_owner_signal,NULL,&report);
        /* Always unblock our owned queue gate, including early validation errors. */
        mag_render_test_owner_signal(NULL);mag_render_test_owner_close(report.pending==0);
        NSDictionary *result=@{@"device":deviceName,@"status":@(status),@"submitted":@(report.submitted),@"completed":@(report.completed),@"pending":@(report.pending),@"retained_before_terminal":@(report.retained_before_terminal),@"retained_after_terminal":@(report.retained_after_terminal),@"upload_bytes":@(report.upload_bytes)};
        NSData *encoded=[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingSortedKeys error:NULL];
        std::printf("%s\n",[[[NSString alloc] initWithData:encoded encoding:NSUTF8StringEncoding] UTF8String]);
        return status==0 && report.submitted==3 && report.completed==3 && report.pending==0 && report.retained_before_terminal>0 && report.retained_after_terminal==0 ? 0:1;
    }
}

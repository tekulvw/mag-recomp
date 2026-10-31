#import "player_server_overlay.h"
#import <CommonCrypto/CommonDigest.h>
#include "player_archive.h"
#include "sdata_decrypt.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <stdexcept>
#include <cstring>

static NSString *const archivePath=@"PS3_GAME/USRDIR/run/mag/ps3/game.psarc.sdat";
static const std::string settingsMember="/run/mag/ps3/settings/maglocalsettings.lua";
static NSString *sha(NSData *data) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(data.bytes,(CC_LONG)data.length,digest);
    NSMutableString *text=[NSMutableString string];for(unsigned char value:digest)[text appendFormat:@"%02x",value];return text;
}
static void require(BOOL ok,NSString *message){if(!ok)throw std::runtime_error(message.UTF8String);}
static NSString *ipv4(NSString *host) {
    struct in_addr address;
    if(inet_pton(AF_INET,host.UTF8String,&address)==1)return host;
    struct addrinfo hints={},*result=NULL;hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;
    int status=getaddrinfo(host.UTF8String,NULL,&hints,&result);
    require(status==0&&result,[@"The server hostname could not be resolved to IPv4: " stringByAppendingString:host]);
    address=((struct sockaddr_in*)result->ai_addr)->sin_addr;char text[INET_ADDRSTRLEN];
    const char *converted=inet_ntop(AF_INET,&address,text,sizeof text);freeaddrinfo(result);
    require(converted!=NULL,@"The server address could not be converted to IPv4.");
    return [NSString stringWithUTF8String:text];
}
static BOOL ordinary(NSString *path) {
    NSDictionary *attributes=[NSFileManager.defaultManager attributesOfItemAtPath:path error:NULL];
    return [attributes[NSFileType] isEqual:NSFileTypeRegular];
}

NSString *MAGCreateServerOverlay(NSString *sourceRoot,NSString *ownedRoot,NSDictionary *settings,
                                 NSString *expectedMemberSHA256,NSError **error) {
    if(error)*error=nil;
    if(![settings[@"server_address"] length])return sourceRoot;
    NSString *stage=nil;
    @try { try {
        NSFileManager *fm=NSFileManager.defaultManager;
        require(sourceRoot.length&&ownedRoot.length,@"The private server overlay location is unavailable.");
        sourceRoot=sourceRoot.stringByStandardizingPath;
        NSString *destination=[ownedRoot stringByAppendingPathComponent:@"vfs"];
        NSString *markerPath=[ownedRoot stringByAppendingPathComponent:@"server-overlay.json"];
        NSDictionary *identity=@{@"source":sourceRoot,@"server_address":settings[@"server_address"],
            @"tcp_port":settings[@"tcp_port"],@"udp_port":settings[@"udp_port"],@"member_sha256":expectedMemberSHA256};
        if([fm fileExistsAtPath:destination]){
            NSData *markerData=[NSData dataWithContentsOfFile:markerPath];
            NSDictionary *marker=markerData?[NSJSONSerialization JSONObjectWithData:markerData options:0 error:NULL]:nil;
            require([marker isKindOfClass:NSDictionary.class]&&[marker[@"identity"] isEqual:identity],@"The existing private server overlay belongs to different settings.");
            NSString *target=[destination stringByAppendingPathComponent:archivePath];
            require(ordinary(target)&&[marker[@"archive_sha256"] isEqual:sha([NSData dataWithContentsOfFile:target])],@"The private server overlay changed unexpectedly. Relaunch MAG.");
            return destination;
        }
        NSString *source=[sourceRoot stringByAppendingPathComponent:archivePath];
        require(ordinary(source),@"The supported game settings archive is missing.");
        NSDictionary *attributes=[fm attributesOfItemAtPath:source error:NULL];
        require([attributes[NSFileSize] unsignedLongLongValue]<=512ull*1024*1024,@"The game settings archive exceeds its supported size.");
        NSData *raw=[NSData dataWithContentsOfFile:source options:0 error:error];
        require(raw!=nil,@"The game settings archive could not be read.");
        std::vector<uint8_t> archive;
        if(raw.length>=4&&!memcmp(raw.bytes,"NPD\0",4)){
            size_t length=0;uint8_t *plain=sdata_decrypt((const uint8_t*)raw.bytes,raw.length,&length);
            require(plain&&length<=512ull*1024*1024,@"The supported SDATA settings archive could not be decoded.");
            archive.assign(plain,plain+length);free(plain);
        }else{
            const auto *begin=(const uint8_t*)raw.bytes;archive.assign(begin,begin+raw.length);
        }
        auto original=mag::read_psarc_member(archive,settingsMember);
        NSData *originalData=[NSData dataWithBytes:original.data() length:original.size()];
        require([sha(originalData) isEqual:expectedMemberSHA256],@"The settings archive does not match the supported retail version. The original was kept.");
        NSString *host=ipv4(settings[@"server_address"]);
        NSMutableString *overrides=[NSMutableString stringWithFormat:@"\n-- MAG player connection override\nMAGClientNetConfig = MAGClientNetConfig or {}\nMAGClientNetConfig.EnableNetwork = true\nMAGClientNetConfig.MAPSServerAddr = \"%@\"\n",host];
        if([settings[@"tcp_port"] intValue])[overrides appendFormat:@"MAGClientNetConfig.MAPSServerTCPPort = %@\n",settings[@"tcp_port"]];
        if([settings[@"udp_port"] intValue])[overrides appendFormat:@"MAGClientNetConfig.MAPSServerUDPPort = %@\n",settings[@"udp_port"]];
        NSData *extra=[overrides dataUsingEncoding:NSUTF8StringEncoding];
        std::vector<uint8_t> replacement=original;
        replacement.insert(replacement.end(),(const uint8_t*)extra.bytes,(const uint8_t*)extra.bytes+extra.length);
        auto changed=mag::rewrite_psarc_member(archive,settingsMember,replacement);
        NSData *output=[NSData dataWithBytesNoCopy:changed.bytes.data() length:changed.bytes.size() freeWhenDone:NO];
        require([fm createDirectoryAtPath:ownedRoot withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:error],@"The private server overlay folder could not be created.");
        stage=[ownedRoot stringByAppendingPathComponent:[@".vfs-" stringByAppendingString:NSUUID.UUID.UUIDString]];
        require([fm createDirectoryAtPath:stage withIntermediateDirectories:NO attributes:@{NSFilePosixPermissions:@0700} error:error],@"The private server overlay could not be staged.");
        NSArray<NSString*> *parts=archivePath.pathComponents;
        NSString *sourceDir=sourceRoot,*targetDir=stage;
        for(NSUInteger depth=0;depth<parts.count;++depth){
            NSArray *children=[fm contentsOfDirectoryAtPath:sourceDir error:error];
            require(children!=nil,@"The original game folder could not be read.");
            for(NSString *name in children)if(![name isEqual:parts[depth]])
                require([fm createSymbolicLinkAtPath:[targetDir stringByAppendingPathComponent:name]
                   withDestinationPath:[sourceDir stringByAppendingPathComponent:name] error:error],@"The private game view could not be linked.");
            if(depth+1<parts.count){
                sourceDir=[sourceDir stringByAppendingPathComponent:parts[depth]];
                targetDir=[targetDir stringByAppendingPathComponent:parts[depth]];
                require([fm createDirectoryAtPath:targetDir withIntermediateDirectories:NO attributes:@{NSFilePosixPermissions:@0700} error:error],@"The private archive folder could not be created.");
            }
        }
        NSString *target=[stage stringByAppendingPathComponent:archivePath];
        require([output writeToFile:target options:NSDataWritingWithoutOverwriting error:error],@"The private server settings could not be written.");
        require([fm setAttributes:@{NSFilePosixPermissions:@0600} ofItemAtPath:target error:error],@"The private server settings permissions could not be set.");
        NSDictionary *marker=@{@"identity":identity,@"archive_sha256":sha(output)};
        NSData *markerData=[NSJSONSerialization dataWithJSONObject:marker options:0 error:error];
        require(markerData&&[markerData writeToFile:markerPath options:NSDataWritingAtomic error:error],@"The private server overlay marker could not be saved.");
        require([fm moveItemAtPath:stage toPath:destination error:error],@"The private server overlay could not be completed.");
        stage=nil;return destination;
    }catch(const std::exception& failure){
        if(error)*error=[NSError errorWithDomain:@"MAGSettings" code:2 userInfo:@{NSLocalizedDescriptionKey:[NSString stringWithUTF8String:failure.what()]}];
    }}@catch(NSException *exception){
        if(error)*error=[NSError errorWithDomain:@"MAGSettings" code:2 userInfo:@{NSLocalizedDescriptionKey:exception.reason?:@"The server settings could not be prepared."}];
    }
    if(stage)[NSFileManager.defaultManager removeItemAtPath:stage error:NULL];return nil;
}

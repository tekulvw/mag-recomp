#import "player_server_overlay.h"
#import "player_settings_store.h"
#include <assert.h>
int main(int argc,char**argv){@autoreleasepool{
    assert(argc==4);NSMutableDictionary *settings=MAGSettingsDefaults().mutableCopy;
    settings[@"server_address"]=@"127.0.0.1";settings[@"tcp_port"]=@54321;settings[@"udp_port"]=@54322;
    NSError *error=nil;
    NSString *result=MAGCreateServerOverlay([NSString stringWithUTF8String:argv[1]],
        [NSString stringWithUTF8String:argv[2]],settings,[NSString stringWithUTF8String:argv[3]],&error);
    if(!result){fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);return 1;}
    puts(result.fileSystemRepresentation);return 0;
}}

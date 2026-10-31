#import "player_settings_store.h"
#import <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static id invalid(NSError **error, NSString *message) {
    if (error) *error = [NSError errorWithDomain:@"MAGSettings" code:1
                                      userInfo:@{NSLocalizedDescriptionKey:message}];
    return nil;
}

NSDictionary *MAGSettingsDefaults(void) {
    return @{@"schema_version":@1, @"server_address":@"play.magtest.online", @"tcp_port":@0, @"udp_port":@0,
             @"display_mode":@"windowed", @"output_width":@1280, @"output_height":@720,
             @"anti_aliasing":@"off"};
}

static BOOL integer(id value, NSInteger low, NSInteger high) {
    return [value isKindOfClass:NSNumber.class] &&
           CFGetTypeID((__bridge CFTypeRef)value) != CFBooleanGetTypeID() &&
           isfinite([value doubleValue]) && [value doubleValue] == [value longLongValue] &&
           [value longLongValue] >= low && [value longLongValue] <= high;
}

NSDictionary *MAGSettingsValidate(id value, NSError **error) {
    if (error) *error = nil;
    if (![value isKindOfClass:NSDictionary.class]) return invalid(error, @"Settings must contain an object.");
    NSDictionary *d = value;
    if (!integer(d[@"schema_version"],1,1)) return invalid(error, @"This settings version is not supported.");
    if (d.count != MAGSettingsDefaults().count) return invalid(error, @"Settings contain missing or unsupported fields.");
    if (![d[@"server_address"] isKindOfClass:NSString.class]) return invalid(error, @"Enter a server hostname or IPv4 address.");
    NSString *host = [d[@"server_address"] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].lowercaseString;
    if (host.length > 253) return invalid(error, @"The server address is too long.");
    if (host.length) {
        struct in_addr address;
        if (inet_pton(AF_INET,host.UTF8String,&address) != 1) {
            NSCharacterSet *allowed = [NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyz0123456789-."];
            NSCharacterSet *numeric = [NSCharacterSet characterSetWithCharactersInString:@"0123456789."];
            if ([host rangeOfCharacterFromSet:allowed.invertedSet].location != NSNotFound ||
                [host rangeOfCharacterFromSet:numeric.invertedSet].location == NSNotFound)
                return invalid(error, @"Use a hostname or IPv4 address, without a URL, path or port.");
            for (NSString *label in [host componentsSeparatedByString:@"."])
                if (!label.length || label.length > 63 || [label hasPrefix:@"-"] || [label hasSuffix:@"-"])
                    return invalid(error, @"Enter a valid server hostname.");
        }
    }
    if (!integer(d[@"tcp_port"],0,65535) || !integer(d[@"udp_port"],0,65535))
        return invalid(error, @"Ports must be between 1 and 65535, or blank for the game default.");
    if (![@[@"windowed",@"borderless"] containsObject:d[@"display_mode"]])
        return invalid(error, @"Choose windowed or borderless fullscreen.");
    if (!integer(d[@"output_width"],1,3840) || !integer(d[@"output_height"],1,2160))
        return invalid(error, @"Choose a supported output size.");
    NSString *size = [NSString stringWithFormat:@"%@x%@",d[@"output_width"],d[@"output_height"]];
    if (![@[@"1280x720",@"1920x1080",@"2560x1440",@"3840x2160"] containsObject:size])
        return invalid(error, @"Choose a supported output size.");
    if (![@[@"off",@"fxaa"] containsObject:d[@"anti_aliasing"]])
        return invalid(error, @"Choose Off or FXAA for anti-aliasing.");
    NSMutableDictionary *normalized = d.mutableCopy;
    normalized[@"server_address"] = host;
    return normalized.copy;
}

NSDictionary *MAGSettingsLoad(NSString *path, BOOL *exists, NSError **error) {
    if (error) *error = nil;
    if (exists) *exists = YES;
    int fd = open(path.fileSystemRepresentation,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) { if (exists) *exists = NO; return MAGSettingsDefaults(); }
        return invalid(error, @"Settings could not be opened. Check the file and its permissions.");
    }
    struct stat st;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size > 65536) {
        close(fd); return invalid(error, @"The settings file is invalid or too large.");
    }
    NSMutableData *data = [NSMutableData data]; unsigned char buffer[4096]; ssize_t n;
    while ((n=read(fd,buffer,sizeof buffer))>0) {
        [data appendBytes:buffer length:(NSUInteger)n];
        if (data.length > 65536) { close(fd); return invalid(error, @"The settings file is too large."); }
    }
    close(fd);
    if (n<0) return invalid(error, @"The settings file could not be read.");
    id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (!object) return invalid(error, @"The settings file is damaged. Review these defaults and Apply to replace it.");
    return MAGSettingsValidate(object,error);
}

BOOL MAGSettingsSave(NSString *path, NSDictionary *settings, NSError **error) {
    NSDictionary *normalized = MAGSettingsValidate(settings,error);
    if (!normalized) return NO;
    NSData *data = [NSJSONSerialization dataWithJSONObject:normalized options:NSJSONWritingPrettyPrinted|NSJSONWritingSortedKeys error:error];
    if (!data) return NO;
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES
                       attributes:@{NSFilePosixPermissions:@0700} error:error]) return NO;
    struct stat st;
    if (!lstat(path.fileSystemRepresentation,&st) && !S_ISREG(st.st_mode)) {
        invalid(error,@"Settings must be an ordinary file."); return NO;
    }
    NSString *pattern = [path stringByAppendingString:@".next.XXXXXX"];
    char *name = strdup(pattern.fileSystemRepresentation); int fd = mkstemp(name);
    if (fd<0) { free(name); invalid(error,@"Cannot create the settings file. Check folder permissions."); return NO; }
    const unsigned char *bytes = data.bytes; NSUInteger left = data.length;
    BOOL ok = YES;
    while (left) { ssize_t n = write(fd,bytes,left); if (n<0 && errno==EINTR) continue;
        if (n<=0) { ok=NO; break; } bytes+=n;left-=(NSUInteger)n; }
    if (ok && fsync(fd)) ok=NO;
    if (close(fd)) ok=NO;
    if (ok && rename(name,path.fileSystemRepresentation)) ok=NO;
    if (!ok) { unlink(name); invalid(error,@"Settings could not be saved. Your previous settings were kept."); }
    free(name); return ok;
}

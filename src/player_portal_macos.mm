// The portal on macOS: an NSURLSession transport for src/player_portal.cpp, and the settings
// panel's account link (MAGAccountLinking in player_settings_macos.h).
#import "player_portal_macos.h"
#include "player_portal.h"
#include <atomic>
#include <cstdio>
#include <memory>

// Redirects are refused: a reply must come from the portal itself, so the device token in the
// Authorization header never follows a redirect elsewhere.
@interface MAGPortalSessionDelegate : NSObject <NSURLSessionTaskDelegate>
@end
@implementation MAGPortalSessionDelegate
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task willPerformHTTPRedirection:(NSHTTPURLResponse *)response
        newRequest:(NSURLRequest *)request completionHandler:(void (^)(NSURLRequest *))completionHandler {
    completionHandler(nil);
}
@end

namespace mag::portal {
namespace {
// Synchronous POST: the caller is the launch preparation or a background link thread. The
// completion runs on the session's own queue, so waiting on the main thread is safe. NSURLSession
// verifies the certificate and host name (App Transport Security: TLS 1.2 or newer).
class SessionTransport final : public Transport {
public:
    Response post(const std::string &url, const std::string &body, const Headers &headers) override {
        @autoreleasepool {
            Response response;
            NSURL *address = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
            if (!address) { response.error = "the portal address is not a valid URL"; return response; }
            NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:address
                                                                   cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                               timeoutInterval:20];
            request.HTTPMethod = @"POST";
            request.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];
            [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
            [request setValue:@"application/json" forHTTPHeaderField:@"Accept"];
            [request setValue:@"MAG-launcher" forHTTPHeaderField:@"User-Agent"];
            for (const auto &[name, value] : headers)
                [request setValue:[NSString stringWithUTF8String:value.c_str()] forHTTPHeaderField:[NSString stringWithUTF8String:name.c_str()]];
            NSURLSessionConfiguration *configuration = NSURLSessionConfiguration.ephemeralSessionConfiguration;
            configuration.URLCache = nil; configuration.HTTPCookieStorage = nil; configuration.URLCredentialStorage = nil;
            configuration.timeoutIntervalForRequest = 20; configuration.timeoutIntervalForResource = 40;
            NSURLSession *session = [NSURLSession sessionWithConfiguration:configuration delegate:[MAGPortalSessionDelegate new] delegateQueue:nil];
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            __block NSInteger status = 0; __block NSData *data = nil; __block NSString *failure = nil, *retry = nil;
            [[session dataTaskWithRequest:request completionHandler:^(NSData *reply, NSURLResponse *meta, NSError *error) {
                if (error) failure = error.localizedDescription ?: @"the request failed";
                else if ([meta isKindOfClass:NSHTTPURLResponse.class]) {
                    status = ((NSHTTPURLResponse *)meta).statusCode; data = reply;
                    retry = [((NSHTTPURLResponse *)meta) valueForHTTPHeaderField:@"Retry-After"];
                }
                else failure = @"the reply was not HTTP";
                dispatch_semaphore_signal(done);
            }] resume];
            dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
            [session finishTasksAndInvalidate];
            if (failure) { response.error = failure.UTF8String; return response; }
            response.status = int(status);
            if (retry) response.retry_after = parse_retry_after(retry.UTF8String);
            if (data.length > (1u << 20)) { response.status = 0; response.error = "the reply was too large"; return response; }
            response.body.assign((const char *)data.bytes, data.length);
            return response;
        }
    }
};
}  // namespace

std::unique_ptr<Transport> make_platform_transport() { return std::make_unique<SessionTransport>(); }
}  // namespace mag::portal

static NSString *string(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

@implementation MAGPortalAccount {
    std::filesystem::path _linkFile;
    std::shared_ptr<std::atomic<bool>> _cancel;
}

- (instancetype)initWithSettingsPath:(NSString *)settingsPath {
    if (!(self = [super init])) return nil;
    _linkFile = mag::portal::link_path(settingsPath.fileSystemRepresentation);
    return self;
}

- (NSString *)portalForServer:(NSString *)server portalURL:(NSString *)portalURL {
    std::string setting = portalURL.UTF8String ?: "", error;
    if (!mag::portal::check_portal_url(setting, error)) return nil;
    std::string portal = mag::portal::portal_for(server.lowercaseString.UTF8String ?: "", setting);
    return portal.empty() ? nil : string(portal);
}

- (NSString *)linkedNameForPortal:(NSString *)portal {
    mag::portal::Link link; bool exists = false; std::string error;
    if (!mag::portal::load_link(_linkFile, link, exists, error) || !exists || link.portal != (portal.UTF8String ?: "")) return nil;
    return string(link.player_name);
}

- (void)startLinkWithPortal:(NSString *)portal progress:(void (^)(NSString *))progress finished:(void (^)(BOOL, NSString *))finished {
    if (_cancel) _cancel->store(true);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    _cancel = cancel;
    const std::string address = portal.UTF8String ?: "";
    const auto file = _linkFile;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto transport = mag::portal::make_platform_transport();
        mag::portal::Link link;
        mag::portal::LinkUi ui;
        ui.show = [&](const mag::portal::LinkStart &start) {
            NSString *message = [NSString stringWithFormat:@"Enter %@ at %@ (opened in your browser)", string(start.user_code), string(start.verification_uri)];
            NSURL *page = [NSURL URLWithString:string(start.verification_uri_complete.empty() ? start.verification_uri : start.verification_uri_complete)];
            dispatch_async(dispatch_get_main_queue(), ^{
                if (page) [NSWorkspace.sharedWorkspace openURL:page];
                progress(message);
            });
        };
        ui.wait = [&](int seconds) {
            for (int tick = 0; tick < seconds * 4; ++tick) {
                if (cancel->load()) return false;
                [NSThread sleepForTimeInterval:0.25];
            }
            return !cancel->load();
        };
        mag::portal::Outcome outcome = mag::portal::run_link(*transport, address, "MAG launcher (macOS)", ui, link);
        std::string error;
        if (outcome.ok() && !mag::portal::save_link(file, link, error)) outcome = {mag::portal::Status::Storage, error};
        const BOOL linked = outcome.ok();
        NSString *message = linked ? @"" : string(outcome.message);
        dispatch_async(dispatch_get_main_queue(), ^{ finished(linked, outcome.status == mag::portal::Status::Cancelled ? @"" : message); });
    });
}

- (void)cancelLink { if (_cancel) _cancel->store(true); }

- (BOOL)unlink:(NSString **)message {
    mag::portal::Link link; bool exists = false; std::string error;
    mag::portal::load_link(_linkFile, link, exists, error);
    if (!mag::portal::forget_link(_linkFile, error)) { if (message) *message = string(error); return NO; }
    if (message) *message = link.portal.empty() ? @"Not linked on this computer"
                                                : [NSString stringWithFormat:@"Unlinked here · also remove it at %@/account", string(link.portal)];
    return YES;
}
@end

NSString *MAGPortalLaunchTicket(NSString *settingsPath, NSString *ownedFolder, NSDictionary *settings, NSString **playerName,
                                NSString **message, BOOL *failed) {
    *playerName = nil; *message = nil; *failed = NO;
    NSString *server = settings[@"server_address"];
    if (!server.length || [settings[@"ticket_file"] length] || !ownedFolder.length) return nil;
    const std::string portal = mag::portal::portal_for(server.UTF8String, [settings[@"portal_url"] UTF8String] ?: "");
    if (portal.empty()) return nil;
    auto transport = mag::portal::make_platform_transport();
    const std::filesystem::path ticket = std::filesystem::path(ownedFolder.fileSystemRepresentation) / "portal-ticket.bin";
    auto result = mag::portal::prepare_launch_ticket(*transport, mag::portal::link_path(settingsPath.fileSystemRepresentation), ticket, portal,
                                                     server.UTF8String,
                                                     "Link it again in MAG Settings (MAG > Settings…, Account).");
    using State = mag::portal::LaunchTicket::State;
    if (!result.message.empty()) *message = string(result.message);
    *failed = result.state == State::Failed;
    static std::unique_ptr<mag::portal::TicketRefill> refill;
    refill.reset();
    if (result.state != State::Ready) return nil;
    // Each later sign-in of this phase (a reconnect, a return to the lobby) takes a fresh ticket
    // from the file (PS3RECOMP_NP_TICKET_REFILL), which this refill keeps filled.
    refill = mag::portal::start_ticket_refill(mag::portal::make_platform_transport, mag::portal::link_path(settingsPath.fileSystemRepresentation),
                                              ticket, portal, server.UTF8String,
                                              "Link it again in MAG Settings (MAG > Settings…, Account).", result.expires_at,
                                              [](const std::string &line) { std::fprintf(stderr, "[mag account] %s\n", line.c_str()); });
    *playerName = string(result.player_name);
    return [NSString stringWithUTF8String:result.file.c_str()];
}

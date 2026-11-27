// macOS microphone access (AVFoundation). The app bundle's Info.plist carries the usage string
// (NSMicrophoneUsageDescription, installer/mag_installer/native_package.py); without it macOS
// would end the process on the first capture instead of asking.
#include "mag_voice_permission.h"

#import <AVFoundation/AVFoundation.h>

#include <atomic>

namespace mag::voice {
MicrophonePermission microphone_permission() {
    static std::atomic<bool> asked{false};
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
    case AVAuthorizationStatusAuthorized:
        return MicrophonePermission::Allowed;
    case AVAuthorizationStatusDenied:
    case AVAuthorizationStatusRestricted:
        return MicrophonePermission::Denied;
    case AVAuthorizationStatusNotDetermined:
    default:
        if (!asked.exchange(true))
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL) {}];
        return MicrophonePermission::Asking;
    }
}
}  // namespace mag::voice

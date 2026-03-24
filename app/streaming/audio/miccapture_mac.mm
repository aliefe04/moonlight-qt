/**
 * @file miccapture_mac.mm
 * @brief macOS Objective-C++ bridge for AVFoundation microphone permission.
 *
 * This file must be compiled as Objective-C++ (.mm) because AVFoundation
 * headers use Objective-C syntax incompatible with plain C++.
 */

#import <AVFoundation/AVFoundation.h>

extern "C" int MicCapture_checkPermissionStatus_mac()
{
    if (@available(macOS 10.14, *)) {
        AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
        switch (status) {
            case AVAuthorizationStatusAuthorized:
                return 1;  // Permission granted
            case AVAuthorizationStatusDenied:
                return -1; // Permission denied
            case AVAuthorizationStatusRestricted:
                return -2; // Permission restricted (parental controls)
            case AVAuthorizationStatusNotDetermined:
            default:
                return 0;  // Not asked yet
        }
    }
    return 1;  // Older macOS versions don't have permission system
}

extern "C" void MicCapture_requestPermission_mac(void (*callback)(int granted))
{
    if (@available(macOS 10.14, *)) {
        AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
        
        if (status == AVAuthorizationStatusAuthorized) {
            // Already granted
            if (callback) callback(1);
            return;
        }
        
        if (status == AVAuthorizationStatusDenied || status == AVAuthorizationStatusRestricted) {
            // Already denied or restricted
            if (callback) callback(0);
            return;
        }
        
        // Not determined - ask for permission
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                   completionHandler:^(BOOL granted) {
            // Callback runs on background thread
            if (callback) {
                callback(granted ? 1 : 0);
            }
        }];
    } else {
        // Older macOS - no permission system
        if (callback) callback(1);
    }
}
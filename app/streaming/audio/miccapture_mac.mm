/**
 * @file miccapture_mac.mm
 * @brief macOS Objective-C++ bridge for AVFoundation microphone permission.
 *
 * This file must be compiled as Objective-C++ (.mm) because AVFoundation
 * headers use Objective-C syntax incompatible with plain C++.
 */

#import <AVFoundation/AVFoundation.h>

extern "C" void MicCapture_requestPermission_mac()
{
    if (@available(macOS 10.14, *)) {
        // Trigger the system microphone permission dialog (or silently succeed
        // if permission was already granted). The completion handler fires on a
        // background thread — we don't need to act on it; SDL will get the
        // correct result the next time SDL_OpenAudioDevice is called.
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                               completionHandler:^(BOOL) {}];
    }
}

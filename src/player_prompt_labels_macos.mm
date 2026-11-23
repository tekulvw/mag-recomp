/* macOS host key names for prompt labels (player_prompt_labels.h). The Text Input Sources API
 * belongs to the main thread, so the names are read there with UCKeyTranslate into a table, at
 * the first query and again whenever the selected keyboard layout changes; other threads read
 * the table. Until it is filled the portable US labels apply. */
#include "player_prompt_labels.h"
#import <Carbon/Carbon.h>
#import <Foundation/Foundation.h>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace {
// Virtual key codes of the layout-dependent keys, by HID usage; -1: not one of them.
int key_code(uint32_t usage) {
    static const uint8_t letters[26] = {0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2E,
                                        0x2D, 0x1F, 0x23, 0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07, 0x10, 0x06};
    static const uint8_t digits[10] = {0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19, 0x1D};  // 1..9, 0
    if (usage >= 4 && usage <= 29) return letters[usage - 4];
    if (usage >= 30 && usage <= 39) return digits[usage - 30];
    switch (usage) {
    case 45: return 0x1B; case 46: return 0x18; case 47: return 0x21; case 48: return 0x1E;
    case 49: return 0x2A; case 50: return 0x2A; case 51: return 0x29; case 52: return 0x27;
    case 53: return 0x32; case 54: return 0x2B; case 55: return 0x2F; case 56: return 0x2C;
    case 100: return 0x0A;
    default: return -1;
    }
}

std::mutex table_lock;
std::array<std::string, 256> table;
std::atomic<bool> requested{false};

void fill_table() {  // main thread
    std::array<std::string, 256> next;
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    CFDataRef data = source ? (CFDataRef)TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData) : nullptr;
    if (data) {
        const auto* layout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(data));
        for (uint32_t usage = 0; usage < next.size(); ++usage) {
            const int code = key_code(usage);
            if (code < 0) continue;
            UInt32 dead = 0;
            UniChar chars[8]{};
            UniCharCount length = 0;
            if (UCKeyTranslate(layout, UInt16(code), kUCKeyActionDisplay, 0, LMGetKbdType(),
                               kUCKeyTranslateNoDeadKeysMask, &dead, 8, &length, chars) != noErr || !length)
                continue;
            NSString* text = [NSString stringWithCharacters:chars length:length];
            if (const char* utf8 = text.UTF8String) next[usage] = utf8;
        }
    }
    if (source) CFRelease(source);
    std::lock_guard lock(table_lock);
    table = std::move(next);
}

void layout_changed(CFNotificationCenterRef, void*, CFStringRef, const void*, CFDictionaryRef) {
    dispatch_async(dispatch_get_main_queue(), ^{ fill_table(); });
}

void request_table() {
    if (requested.exchange(true)) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        CFNotificationCenterAddObserver(CFNotificationCenterGetDistributedCenter(), nullptr, layout_changed,
                                        kTISNotifySelectedKeyboardInputSourceChanged, nullptr,
                                        CFNotificationSuspensionBehaviorDeliverImmediately);
        fill_table();
    });
}
}  // namespace

extern "C" size_t mag_prompt_host_key_name(uint32_t usage, char* out, size_t capacity) {
    if (!out || !capacity || usage >= table.size() || key_code(usage) < 0) return 0;
    request_table();
    std::lock_guard lock(table_lock);
    const std::string& name = table[usage];
    if (name.empty() || name.size() >= capacity) return 0;
    std::memcpy(out, name.data(), name.size());
    return name.size();
}

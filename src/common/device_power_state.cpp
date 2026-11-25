// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "device_power_state.h"

#if defined(_WIN32)
#include <windows.h>

#elif defined(__ANDROID__)
#include <atomic>

#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_MAC
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#endif

#elif defined(__linux__)
#include <fstream>
#include <string>
#include <dirent.h>
#endif

namespace Common {

#ifdef __ANDROID__
namespace {
std::atomic<PowerStatus> android_power_status{PowerStatus{100, false, true}};
}

void SetAndroidPowerStatus(PowerStatus status) {
    android_power_status.store(status, std::memory_order_relaxed);
}
#endif

    PowerStatus GetPowerStatus()
    {
        PowerStatus info;

#if defined(_WIN32)
        SYSTEM_POWER_STATUS status;
        if (GetSystemPowerStatus(&status)) {
            if (status.BatteryFlag == 128) {
                info.has_battery = false;
            } else {
                info.percentage = status.BatteryLifePercent;
                info.charging = (status.BatteryFlag & 8) != 0;
            }
        } else {
            info.has_battery = false;
        }

#elif defined(__ANDROID__)
        info = android_power_status.load(std::memory_order_relaxed);

#elif defined(__APPLE__) && TARGET_OS_MAC
        info.has_battery = false;
        CFTypeRef info_ref = IOPSCopyPowerSourcesInfo();
        if (info_ref != nullptr) {
            CFArrayRef sources = IOPSCopyPowerSourcesList(info_ref);
            if (sources != nullptr) {
                const CFIndex count = CFArrayGetCount(sources);
                for (CFIndex i = 0; i < count; ++i) {
                    CFTypeRef source = CFArrayGetValueAtIndex(sources, i);
                    if (source == nullptr) {
                        continue;
                    }
                    CFDictionaryRef battery = IOPSGetPowerSourceDescription(info_ref, source);
                    if (battery == nullptr) {
                        continue;
                    }
                    CFTypeRef ps_type = CFDictionaryGetValue(battery, CFSTR(kIOPSTypeKey));
                    if (ps_type == nullptr || CFGetTypeID(ps_type) != CFStringGetTypeID() ||
                        !CFEqual(ps_type, CFSTR(kIOPSInternalBatteryType))) {
                        continue;
                    }
                    CFTypeRef is_present = CFDictionaryGetValue(battery, CFSTR(kIOPSIsPresentKey));
                    if (is_present != nullptr && CFGetTypeID(is_present) == CFBooleanGetTypeID() &&
                        CFBooleanGetValue(static_cast<CFBooleanRef>(is_present)) == 0) {
                        continue;
                    }

                    info.has_battery = true;

                    CFTypeRef cur_ref =
                        CFDictionaryGetValue(battery, CFSTR(kIOPSCurrentCapacityKey));
                    CFTypeRef max_ref =
                        CFDictionaryGetValue(battery, CFSTR(kIOPSMaxCapacityKey));
                    int cur = 0;
                    int max = 0;
                    if (cur_ref != nullptr && CFGetTypeID(cur_ref) == CFNumberGetTypeID() &&
                        max_ref != nullptr && CFGetTypeID(max_ref) == CFNumberGetTypeID() &&
                        CFNumberGetValue(static_cast<CFNumberRef>(cur_ref), kCFNumberIntType,
                                         &cur) != 0 &&
                        CFNumberGetValue(static_cast<CFNumberRef>(max_ref), kCFNumberIntType,
                                         &max) != 0 &&
                        cur >= 0 && max > 0) {
                        info.percentage = (cur * 100) / max;
                    }

                    CFTypeRef is_charging =
                        CFDictionaryGetValue(battery, CFSTR(kIOPSIsChargingKey));
                    if (is_charging != nullptr &&
                        CFGetTypeID(is_charging) == CFBooleanGetTypeID()) {
                        info.charging =
                            CFBooleanGetValue(static_cast<CFBooleanRef>(is_charging)) != 0;
                    }
                    break;
                }
                CFRelease(sources);
            }
            CFRelease(info_ref);
        }

#elif defined(__linux__)
        const char* battery_path = "/sys/class/power_supply/BAT0/";

        std::ifstream capFile(std::string(battery_path) + "capacity");
        if (capFile) {
            capFile >> info.percentage;
        }
        else {
            info.has_battery = false;
        }

        std::ifstream statFile(std::string(battery_path) + "status");
        if (statFile) {
            std::string status;
            std::getline(statFile, status);
            info.charging = (status == "Charging");
        }
#else
        info.has_battery = false;
#endif

        return info;
    }
}

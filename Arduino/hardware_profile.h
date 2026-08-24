#ifndef EPF_HARDWARE_PROFILE_H
#define EPF_HARDWARE_PROFILE_H

// Select one profile with a PlatformIO build flag, for example:
//   -DEPF_HARDWARE_PROFILE=EPF_PROFILE_PROTOTYPE_OLED
//
// The default is the planned EE04 e-paper target when the sketch is opened
// directly in Arduino IDE.
#define EPF_PROFILE_PROTOTYPE_OLED 1
#define EPF_PROFILE_EE04_EPAPER 2

#ifndef EPF_HARDWARE_PROFILE
#define EPF_HARDWARE_PROFILE EPF_PROFILE_EE04_EPAPER
#endif

#if EPF_HARDWARE_PROFILE == EPF_PROFILE_PROTOTYPE_OLED
#define EPF_USE_OLED 1
#define EPF_USE_EPAPER 0
#define EPF_HAS_BATTERY_MONITOR 0
#define EPF_ENABLE_DEEP_SLEEP 0
#define EPF_HAS_CONFIG_BUTTON 0
#define EPF_DEFAULT_TAILSCALE 0
#define EPF_DEFAULT_SECURE_HTTP 1
#define EPF_PROTOTYPE_FAKE_BATTERY_MV 3808
#elif EPF_HARDWARE_PROFILE == EPF_PROFILE_EE04_EPAPER
#define EPF_USE_OLED 0
#define EPF_USE_EPAPER 1
#define EPF_HAS_BATTERY_MONITOR 1
#define EPF_ENABLE_DEEP_SLEEP 1
#define EPF_HAS_CONFIG_BUTTON 1
#define EPF_DEFAULT_TAILSCALE 0
#define EPF_DEFAULT_SECURE_HTTP 1
#else
#error "Unknown EPF_HARDWARE_PROFILE"
#endif

// Build environments can override this per device. Keeping it compile-time
// means a local frame does not include or start the Tailscale client at all.
#ifndef EPF_ENABLE_TAILSCALE
#define EPF_ENABLE_TAILSCALE EPF_DEFAULT_TAILSCALE
#endif

#ifndef EPF_ENABLE_SECURE_HTTP
#define EPF_ENABLE_SECURE_HTTP EPF_DEFAULT_SECURE_HTTP
#endif

#endif

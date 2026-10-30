/*
 * Virtual gamepads for ihslib's HID channel.
 *
 * Steam receives every device as a generic gamepad. The report is the same 48-byte layout
 * ihslib's SDL backend sends: six int16 axes and sixteen buttons in SDL_GameController
 * order. Input code publishes state per slot; ihslib's 125 Hz HID poll picks it up and
 * sends deltas, so callers never touch ihslib's device locks.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "ihslib/hid.h"

#define TSL_GAMEPAD_SLOTS 4
#define TSL_GAMEPAD_REPORT_SIZE 48

/* Button bits, SDL_GameControllerButton order. */
typedef enum TSL_GamepadButton {
    TSL_PAD_A = 0,
    TSL_PAD_B = 1,
    TSL_PAD_X = 2,
    TSL_PAD_Y = 3,
    TSL_PAD_BACK = 4,
    TSL_PAD_GUIDE = 5,
    TSL_PAD_START = 6,
    TSL_PAD_LEFT_STICK = 7,
    TSL_PAD_RIGHT_STICK = 8,
    TSL_PAD_LEFT_SHOULDER = 9,
    TSL_PAD_RIGHT_SHOULDER = 10,
    TSL_PAD_DPAD_UP = 11,
    TSL_PAD_DPAD_DOWN = 12,
    TSL_PAD_DPAD_LEFT = 13,
    TSL_PAD_DPAD_RIGHT = 14,
    TSL_PAD_MISC1 = 15,
} TSL_GamepadButton;

/* Axis indexes, SDL_GameControllerAxis order. Sticks are -32768..32767 with Y pointing
 * down, triggers 0..32767. */
typedef enum TSL_GamepadAxis {
    TSL_PAD_AXIS_LEFT_X = 0,
    TSL_PAD_AXIS_LEFT_Y = 1,
    TSL_PAD_AXIS_RIGHT_X = 2,
    TSL_PAD_AXIS_RIGHT_Y = 3,
    TSL_PAD_AXIS_TRIGGER_LEFT = 4,
    TSL_PAD_AXIS_TRIGGER_RIGHT = 5,
} TSL_GamepadAxis;

typedef struct TSL_GamepadState {
    uint16_t buttons;
    int16_t axes[6];
} TSL_GamepadState;

typedef struct TSL_GamepadInfo {
    uint16_t vendorId;
    uint16_t productId;
    char name[64];
} TSL_GamepadInfo;

/* Called from ihslib's session thread when Steam asks a pad to rumble. Magnitudes are
 * 0..65535. */
typedef void (TSL_GamepadRumbleFunction)(int slot, uint16_t lowFreq, uint16_t highFreq, uint32_t durationMs,
                                         void *context);

/*
 * Convert a W3C "standard" mapping gamepad (navigator.getGamepads()) into SDL order.
 * axes: [LX, LY, RX, RY] in -1..1. values: button values 0..1 (triggers are 6 and 7).
 * pressed: button pressed flags. Missing entries count as released / centered.
 */
void TSL_GamepadStateFromStandard(const double *axes, size_t numAxes, const double *values, const bool *pressed,
                                  size_t numButtons, TSL_GamepadState *out);

/* Vendor and product id from a browser Gamepad.id string. Chromium writes
 * "Name (STANDARD GAMEPAD Vendor: 045e Product: 02fd)", Firefox "045e-02fd-Name".
 * Returns false (and leaves the outputs at 0) when neither form matches. */
bool TSL_GamepadParseIds(const char *id, uint16_t *vendorId, uint16_t *productId);

/* Vendor and product id from a controller's name, for browsers that report only the name
 * in Gamepad.id (Samsung TVs do). Matches well-known names case-insensitively; returns
 * false (and leaves the outputs at 0) for anything else. */
bool TSL_GamepadGuessIds(const char *name, uint16_t *vendorId, uint16_t *productId);

/* Steam's EControllerType for a vendor and product id, so Big Picture and games show the
 * right button prompts. Pads it doesn't know are k_ControllerTypeUnknownNonSteamController. */
int32_t TSL_GamepadControllerType(uint16_t vendorId, uint16_t productId);

/* Pack a state into the 48-byte report. reportVersion is the byte Steam sets through
 * the "requested report version" write command. */
void TSL_GamepadPackReport(const TSL_GamepadState *state, uint8_t reportVersion,
                           uint8_t out[TSL_GAMEPAD_REPORT_SIZE]);

IHS_HIDProvider *TSL_GamepadProviderCreate(void);

/* Only after the owning session was destroyed (or the provider never added to one). */
void TSL_GamepadProviderDestroy(IHS_HIDProvider *provider);

void TSL_GamepadProviderSetRumbleCallback(IHS_HIDProvider *provider, TSL_GamepadRumbleFunction *function,
                                          void *context);

/* Returns true when the connected set changed; the caller then tells the session with
 * IHS_SessionHIDNotifyDeviceChange. info may be NULL. Disconnecting clears the slot's state,
 * so an open device reports a released pad until Steam closes it. */
bool TSL_GamepadProviderSetConnected(IHS_HIDProvider *provider, int slot, bool connected,
                                     const TSL_GamepadInfo *info);

void TSL_GamepadProviderUpdate(IHS_HIDProvider *provider, int slot, const TSL_GamepadState *state);

#ifdef __cplusplus
}
#endif

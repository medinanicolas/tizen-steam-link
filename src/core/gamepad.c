#include "gamepad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hid/device.h"
#include "hid/provider.h"
#include "ihs_buffer.h"
#include "ihs_enumeration.h"
#include "ihs_thread.h"

/* Feature report numbers Steam asks for, as answered by ihslib's SDL backend. */
enum {
    FEATURE_POWER_LEVEL = 0x02,
    FEATURE_CAPS = 0x04,
    FEATURE_SERIAL = 0x07,
};

/* Write commands, same numbering as ihslib's SDL backend. */
enum {
    WRITE_RUMBLE = 0x01,
    WRITE_SET_LED = 0x05,
    WRITE_RUMBLE_TRIGGERS = 0x06,
    WRITE_SET_SENSOR_ENABLED = 0x08,
    WRITE_SET_REQUESTED_REPORT_VERSION = 0x09,
    WRITE_SET_PS5_RUMBLE = 0x0a,
    WRITE_SET_PLAYER_INDEX = 0x0b,
};

/* SDL_JOYSTICK_POWER_UNKNOWN */
#define POWER_LEVEL_UNKNOWN 0xFF
/* EControllerType values, same numbering as ihslib's SDL backend. */
#define CONTROLLER_TYPE_GENERIC 30   /* k_ControllerTypeUnknownNonSteamController */
#define CONTROLLER_TYPE_XBOX360 31
#define CONTROLLER_TYPE_XBOXONE 32
#define CONTROLLER_TYPE_PS4 34
#define CONTROLLER_TYPE_SWITCH_PRO 38
#define CONTROLLER_TYPE_PS5 45

int32_t TSL_GamepadControllerType(uint16_t vendorId, uint16_t productId) {
    switch (vendorId) {
        case 0x054c: /* Sony */
            switch (productId) {
                case 0x0ce6: /* DualSense */
                case 0x0df2: /* DualSense Edge */
                    return CONTROLLER_TYPE_PS5;
                case 0x05c4:
                case 0x09cc:
                case 0x0ba0: /* DualShock 4 (v1, v2, USB adapter) */
                    return CONTROLLER_TYPE_PS4;
            }
            break;
        case 0x045e: /* Microsoft */
            if (productId == 0x028e || productId == 0x028f || productId == 0x0719) return CONTROLLER_TYPE_XBOX360;
            return CONTROLLER_TYPE_XBOXONE;
        case 0x057e: /* Nintendo */
            if (productId == 0x2009) return CONTROLLER_TYPE_SWITCH_PRO;
            break;
    }
    return CONTROLLER_TYPE_GENERIC;
}

typedef struct Slot {
    bool connected;
    TSL_GamepadInfo info;
    TSL_GamepadState state;
} Slot;

typedef struct GamepadProvider {
    IHS_HIDProvider base;
    IHS_Mutex *lock;
    Slot slots[TSL_GAMEPAD_SLOTS];
    TSL_GamepadRumbleFunction *rumble;
    void *rumbleContext;
} GamepadProvider;

typedef struct GamepadDevice {
    IHS_HIDDevice base;
    GamepadProvider *provider;
    int slot;
    /* Set by StartInputReports. ihslib sizes delta reports from the length given there, so
     * nothing may be sent before it. */
    bool started;
    uint8_t reportVersion;
    TSL_GamepadState current;
    uint8_t previous[TSL_GAMEPAD_REPORT_SIZE];
} GamepadDevice;

typedef struct DeviceEntry {
    int slot;
    TSL_GamepadInfo info;
} DeviceEntry;

typedef struct DeviceEnumeration {
    IHS_Enumeration base;
    DeviceEntry entries[TSL_GAMEPAD_SLOTS];
    size_t count, index;
    char path[16];
} DeviceEnumeration;

static void WriteLE16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t) (v & 0xFF);
    p[1] = (uint8_t) (v >> 8);
}

static int16_t ScaleStick(double v) {
    if (v != v) return 0;
    if (v <= -1.0) return -32768;
    if (v >= 1.0) return 32767;
    return (int16_t) (v < 0 ? v * 32768.0 : v * 32767.0);
}

static int16_t ScaleTrigger(double v) {
    if (v != v || v <= 0) return 0;
    if (v >= 1.0) return 32767;
    return (int16_t) (v * 32767.0);
}

void TSL_GamepadStateFromStandard(const double *axes, size_t numAxes, const double *values, const bool *pressed,
                                  size_t numButtons, TSL_GamepadState *out) {
    /* W3C standard mapping index -> SDL button bit; -1 for the analog triggers. */
    static const int buttonMap[17] = {
            TSL_PAD_A, TSL_PAD_B, TSL_PAD_X, TSL_PAD_Y,
            TSL_PAD_LEFT_SHOULDER, TSL_PAD_RIGHT_SHOULDER,
            -1, -1,
            TSL_PAD_BACK, TSL_PAD_START,
            TSL_PAD_LEFT_STICK, TSL_PAD_RIGHT_STICK,
            TSL_PAD_DPAD_UP, TSL_PAD_DPAD_DOWN, TSL_PAD_DPAD_LEFT, TSL_PAD_DPAD_RIGHT,
            TSL_PAD_GUIDE,
    };
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < numButtons && i < 17; i++) {
        if (buttonMap[i] >= 0 && pressed != NULL && pressed[i]) {
            out->buttons |= (uint16_t) (1u << buttonMap[i]);
        }
    }
    for (size_t i = 0; i < numAxes && i < 4; i++) {
        out->axes[i] = ScaleStick(axes[i]);
    }
    if (values != NULL) {
        if (numButtons > 6) out->axes[TSL_PAD_AXIS_TRIGGER_LEFT] = ScaleTrigger(values[6]);
        if (numButtons > 7) out->axes[TSL_PAD_AXIS_TRIGGER_RIGHT] = ScaleTrigger(values[7]);
    }
}

static bool ParseHex4(const char *s, uint16_t *out) {
    uint16_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (uint16_t) ((v << 4) | d);
    }
    *out = v;
    return true;
}

bool TSL_GamepadParseIds(const char *id, uint16_t *vendorId, uint16_t *productId) {
    *vendorId = 0;
    *productId = 0;
    if (id == NULL) return false;
    const char *vendor = strstr(id, "Vendor: ");
    const char *product = strstr(id, "Product: ");
    uint16_t v, p;
    if (vendor != NULL && product != NULL && strlen(vendor) >= 12 && strlen(product) >= 13 &&
        ParseHex4(vendor + 8, &v) && ParseHex4(product + 9, &p)) {
        *vendorId = v;
        *productId = p;
        return true;
    }
    if (strlen(id) >= 10 && id[4] == '-' && id[9] == '-' && ParseHex4(id, &v) && ParseHex4(id + 5, &p)) {
        *vendorId = v;
        *productId = p;
        return true;
    }
    return false;
}

/* Lowercase ASCII copy of at most size - 1 characters. */
static void LowerCopy(char *dest, size_t size, const char *src) {
    size_t i = 0;
    for (; i + 1 < size && src[i] != '\0'; i++) {
        char c = src[i];
        dest[i] = (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
    }
    dest[i] = '\0';
}

bool TSL_GamepadGuessIds(const char *name, uint16_t *vendorId, uint16_t *productId) {
    static const struct {
        const char *needle;
        uint16_t vendor, product;
    } known[] = {
            {"dualsense edge", 0x054c, 0x0df2},
            {"dualsense", 0x054c, 0x0ce6},
            {"dualshock 4", 0x054c, 0x09cc},
            {"ps5", 0x054c, 0x0ce6},
            {"ps4", 0x054c, 0x09cc},
            {"xbox 360", 0x045e, 0x028e},
            {"xbox", 0x045e, 0x0b13},
            {"pro controller", 0x057e, 0x2009},
    };
    *vendorId = 0;
    *productId = 0;
    if (name == NULL) return false;
    char lower[128];
    LowerCopy(lower, sizeof(lower), name);
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strstr(lower, known[i].needle) != NULL) {
            *vendorId = known[i].vendor;
            *productId = known[i].product;
            return true;
        }
    }
    /* Sony's own Bluetooth name, used by both the DualShock 4 and the DualSense. The
     * DualSense is the one sold today, so it wins. */
    if (strcmp(lower, "wireless controller") == 0) {
        *vendorId = 0x054c;
        *productId = 0x0ce6;
        return true;
    }
    return false;
}

void TSL_GamepadPackReport(const TSL_GamepadState *state, uint8_t reportVersion,
                           uint8_t out[TSL_GAMEPAD_REPORT_SIZE]) {
    memset(out, 0, TSL_GAMEPAD_REPORT_SIZE);
    for (int i = 0; i < 6; i++) {
        WriteLE16(&out[i * 2], (uint16_t) state->axes[i]);
    }
    /* bytes 12..15: flags, always 0 */
    WriteLE16(&out[16], state->buttons);
    /* bytes 18..26: padding */
    out[27] = reportVersion;
    /* bytes 28..47: gyro, accel, touch, padding; this source has no motion sensors */
}

/* Enumeration of connected slots, snapshot taken under the provider lock. */

static IHS_Enumeration *EnumAlloc(const IHS_EnumerationClass *cls, void *arg) {
    (void) arg;
    DeviceEnumeration *e = calloc(1, sizeof(DeviceEnumeration));
    e->base.cls = cls;
    return (IHS_Enumeration *) e;
}

static void EnumFree(IHS_Enumeration *e) {
    free(e);
}

static size_t EnumCount(const IHS_Enumeration *e) {
    return ((const DeviceEnumeration *) e)->count;
}

static void EnumReset(IHS_Enumeration *e) {
    ((DeviceEnumeration *) e)->index = 0;
}

static bool EnumEnded(const IHS_Enumeration *e) {
    const DeviceEnumeration *de = (const DeviceEnumeration *) e;
    return de->index >= de->count;
}

static void *EnumGet(const IHS_Enumeration *e) {
    const DeviceEnumeration *de = (const DeviceEnumeration *) e;
    if (de->index >= de->count) return NULL;
    return (void *) &de->entries[de->index];
}

static void *EnumNext(IHS_Enumeration *e) {
    DeviceEnumeration *de = (DeviceEnumeration *) e;
    if (de->index < de->count) de->index++;
    return EnumGet(e);
}

static const IHS_EnumerationClass EnumerationClass = {
        .alloc = EnumAlloc,
        .free = EnumFree,
        .count = EnumCount,
        .reset = EnumReset,
        .ended = EnumEnded,
        .get = EnumGet,
        .next = EnumNext,
};

/* Device class */

static IHS_HIDDevice *DeviceAlloc(const IHS_HIDDeviceClass *cls) {
    GamepadDevice *device = calloc(1, sizeof(GamepadDevice));
    device->base.cls = cls;
    return (IHS_HIDDevice *) device;
}

static void DeviceFree(IHS_HIDDevice *device) {
    free(device);
}

static void DeviceOpened(IHS_HIDDevice *device) {
    GamepadDevice *pad = (GamepadDevice *) device;
    TSL_GamepadPackReport(&pad->current, pad->reportVersion, pad->previous);
}

static void DeviceClose(IHS_HIDDevice *device) {
    (void) device;
}

static int DeviceWrite(IHS_HIDDevice *device, const uint8_t *data, size_t dataLen) {
    GamepadDevice *pad = (GamepadDevice *) device;
    if (dataLen < 2) {
        return -1;
    }
    switch (data[0]) {
        case WRITE_RUMBLE: {
            if (dataLen < 9) return -1;
            uint16_t low = (uint16_t) (data[1] | (data[2] << 8));
            uint16_t high = (uint16_t) (data[3] | (data[4] << 8));
            uint32_t duration = (uint32_t) data[5] | ((uint32_t) data[6] << 8) | ((uint32_t) data[7] << 16) |
                                ((uint32_t) data[8] << 24);
            GamepadProvider *provider = pad->provider;
            IHS_MutexLock(provider->lock);
            TSL_GamepadRumbleFunction *rumble = provider->rumble;
            void *context = provider->rumbleContext;
            IHS_MutexUnlock(provider->lock);
            if (rumble != NULL) {
                rumble(pad->slot, low, high, duration, context);
            }
            return 0;
        }
        case WRITE_SET_REQUESTED_REPORT_VERSION:
            pad->reportVersion = data[1];
            return 0;
        case WRITE_SET_LED:
        case WRITE_RUMBLE_TRIGGERS:
        case WRITE_SET_SENSOR_ENABLED:
        case WRITE_SET_PS5_RUMBLE:
        case WRITE_SET_PLAYER_INDEX:
            /* Accepted and ignored: the browser Gamepad API can't drive any of these. */
            return 0;
        default:
            return -1;
    }
}

static int DeviceRead(IHS_HIDDevice *device, IHS_Buffer *dest, size_t length, uint32_t timeoutMs) {
    (void) device;
    (void) timeoutMs;
    uint8_t *pointer = IHS_BufferPointerForAppend(dest, length);
    memset(pointer, 0, length);
    return (int) length;
}

static int DeviceSendFeatureReport(IHS_HIDDevice *device, const uint8_t *data, size_t dataLen) {
    (void) device;
    (void) data;
    (void) dataLen;
    return -1;
}

static int DeviceGetFeatureReport(IHS_HIDDevice *device, const uint8_t *reportNumber, size_t reportNumberLen,
                                  IHS_Buffer *dest, size_t length) {
    (void) length;
    GamepadDevice *pad = (GamepadDevice *) device;
    if (reportNumberLen < 1) return -1;
    switch (reportNumber[0]) {
        case FEATURE_SERIAL: {
            IHS_BufferWriteMem(dest, 0, reportNumber, 1);
            IHS_BufferFillMem(dest, 1, 0, 1);
            break;
        }
        case FEATURE_POWER_LEVEL: {
            uint8_t level = POWER_LEVEL_UNKNOWN;
            IHS_BufferWriteMem(dest, 0, reportNumber, 1);
            IHS_BufferWriteMem(dest, 1, &level, 1);
            break;
        }
        case FEATURE_CAPS: {
            /* Same 20-byte layout as ihslib's SDL backend: valid, xinput, controller type
             * (int32), player index (int32), hid, padding. */
            uint8_t caps[20] = {0};
            GamepadProvider *provider = pad->provider;
            IHS_MutexLock(provider->lock);
            TSL_GamepadInfo info = provider->slots[pad->slot].info;
            IHS_MutexUnlock(provider->lock);
            int32_t controllerType = TSL_GamepadControllerType(info.vendorId, info.productId);
            int32_t playerIndex = pad->slot;
            caps[0] = 1;
            caps[1] = 0;
            memcpy(&caps[2], &controllerType, 4);
            memcpy(&caps[6], &playerIndex, 4);
            caps[10] = 0;
            IHS_BufferWriteMem(dest, 0, reportNumber, 1);
            IHS_BufferWriteMem(dest, 1, caps, sizeof(caps));
            break;
        }
        default:
            IHS_BufferFillMem(dest, 1, 0, 1);
            return 0;
    }
    return 0;
}

static int DeviceEmptyString(IHS_HIDDevice *device, IHS_Buffer *dest) {
    (void) device;
    IHS_BufferWriteMem(dest, 0, (const unsigned char *) "", 1);
    return 0;
}

static int DeviceProductString(IHS_HIDDevice *device, IHS_Buffer *dest) {
    GamepadDevice *pad = (GamepadDevice *) device;
    GamepadProvider *provider = pad->provider;
    char name[64];
    IHS_MutexLock(provider->lock);
    memcpy(name, provider->slots[pad->slot].info.name, sizeof(name));
    IHS_MutexUnlock(provider->lock);
    name[sizeof(name) - 1] = '\0';
    IHS_BufferWriteMem(dest, 0, (const unsigned char *) name, strlen(name) + 1);
    return 0;
}

static void AddFullReport(GamepadDevice *pad) {
    uint8_t report[TSL_GAMEPAD_REPORT_SIZE];
    TSL_GamepadPackReport(&pad->current, pad->reportVersion, report);
    IHS_HIDDeviceReportAddFull(&pad->base, report, TSL_GAMEPAD_REPORT_SIZE);
    memcpy(pad->previous, report, TSL_GAMEPAD_REPORT_SIZE);
}

static int DeviceStartInputReports(IHS_HIDDevice *device, size_t length) {
    (void) length;
    IHS_HIDDeviceLock(device);
    ((GamepadDevice *) device)->started = true;
    AddFullReport((GamepadDevice *) device);
    IHS_HIDDeviceUnlock(device);
    return 0;
}

static int DeviceRequestFullReport(IHS_HIDDevice *device) {
    IHS_HIDDeviceLock(device);
    AddFullReport((GamepadDevice *) device);
    IHS_HIDDeviceUnlock(device);
    return 0;
}

static int DeviceRequestDisconnect(IHS_HIDDevice *device, int method, const uint8_t *data, size_t dataLen) {
    (void) device;
    (void) method;
    (void) data;
    (void) dataLen;
    return 0;
}

/*
 * Runs on ihslib's timer thread with the device lock held.
 *
 * An unplugged pad reports a neutral state (its slot state is cleared) instead of returning
 * -1. A negative result makes ihslib close the device on the timer thread, while Steam's own
 * DeviceClose for it (sent after the device list update) closes it on the session thread:
 * two closes of the same device. Steam's close is the only one.
 */
static int DevicePoll(IHS_HIDDevice *device) {
    GamepadDevice *pad = (GamepadDevice *) device;
    if (!pad->started) {
        return 0;
    }
    GamepadProvider *provider = pad->provider;
    IHS_MutexLock(provider->lock);
    TSL_GamepadState state = provider->slots[pad->slot].state;
    IHS_MutexUnlock(provider->lock);
    pad->current = state;
    uint8_t report[TSL_GAMEPAD_REPORT_SIZE];
    TSL_GamepadPackReport(&state, pad->reportVersion, report);
    if (memcmp(report, pad->previous, TSL_GAMEPAD_REPORT_SIZE) == 0) {
        return 0;
    }
    IHS_HIDDeviceReportAddDelta(device, pad->previous, report, TSL_GAMEPAD_REPORT_SIZE);
    memcpy(pad->previous, report, TSL_GAMEPAD_REPORT_SIZE);
    return 1;
}

static const IHS_HIDDeviceClass DeviceClass = {
        .alloc = DeviceAlloc,
        .free = DeviceFree,
        .opened = DeviceOpened,
        .close = DeviceClose,
        .write = DeviceWrite,
        .read = DeviceRead,
        .sendFeatureReport = DeviceSendFeatureReport,
        .getFeatureReport = DeviceGetFeatureReport,
        .getVendorString = DeviceEmptyString,
        .getProductString = DeviceProductString,
        .getSerialNumberString = DeviceEmptyString,
        .startInputReports = DeviceStartInputReports,
        .requestFullReport = DeviceRequestFullReport,
        .requestDisconnect = DeviceRequestDisconnect,
        .poll = DevicePoll,
};

/* Provider class */

static const char kPathPrefix[] = "tsl://";

static int SlotFromPath(const char *path) {
    if (strncmp(path, kPathPrefix, sizeof(kPathPrefix) - 1) != 0) return -1;
    const char *begin = path + sizeof(kPathPrefix) - 1;
    char *end = NULL;
    long slot = strtol(begin, &end, 10);
    if (end == begin || *end != '\0' || slot < 0 || slot >= TSL_GAMEPAD_SLOTS) return -1;
    return (int) slot;
}

static IHS_HIDProvider *ProviderAlloc(const IHS_HIDProviderClass *cls) {
    GamepadProvider *provider = calloc(1, sizeof(GamepadProvider));
    provider->base.cls = cls;
    provider->lock = IHS_MutexCreate();
    return (IHS_HIDProvider *) provider;
}

static void ProviderFree(IHS_HIDProvider *provider) {
    GamepadProvider *pads = (GamepadProvider *) provider;
    IHS_MutexDestroy(pads->lock);
    free(pads);
}

static bool ProviderSupportsDevice(IHS_HIDProvider *provider, const char *path) {
    (void) provider;
    return SlotFromPath(path) >= 0;
}

static IHS_HIDDevice *ProviderOpenDevice(IHS_HIDProvider *provider, const char *path) {
    GamepadProvider *pads = (GamepadProvider *) provider;
    int slot = SlotFromPath(path);
    if (slot < 0) return NULL;
    IHS_MutexLock(pads->lock);
    bool connected = pads->slots[slot].connected;
    TSL_GamepadState state = pads->slots[slot].state;
    IHS_MutexUnlock(pads->lock);
    if (!connected) return NULL;
    GamepadDevice *device = (GamepadDevice *) IHS_HIDDeviceCreate(&DeviceClass);
    device->provider = pads;
    device->slot = slot;
    device->current = state;
    return (IHS_HIDDevice *) device;
}

static bool ProviderHasChange(IHS_HIDProvider *provider) {
    (void) provider;
    return false;
}

static IHS_Enumeration *ProviderEnumerate(IHS_HIDProvider *provider) {
    GamepadProvider *pads = (GamepadProvider *) provider;
    DeviceEnumeration *e = (DeviceEnumeration *) IHS_EnumerationCreate(&EnumerationClass, NULL);
    IHS_MutexLock(pads->lock);
    for (int i = 0; i < TSL_GAMEPAD_SLOTS; i++) {
        if (!pads->slots[i].connected) continue;
        e->entries[e->count].slot = i;
        e->entries[e->count].info = pads->slots[i].info;
        e->count++;
    }
    IHS_MutexUnlock(pads->lock);
    return (IHS_Enumeration *) e;
}

static void ProviderDeviceInfo(IHS_HIDProvider *provider, IHS_Enumeration *enumeration, IHS_HIDDeviceInfo *info) {
    (void) provider;
    DeviceEnumeration *e = (DeviceEnumeration *) enumeration;
    const DeviceEntry *entry = EnumGet(enumeration);
    if (entry == NULL) return;
    snprintf(e->path, sizeof(e->path), "%s%d", kPathPrefix, entry->slot);
    info->path = e->path;
    info->product_string = entry->info.name[0] ? entry->info.name : "Generic Gamepad";
    info->serial_number = NULL;
    info->vendor_id = entry->info.vendorId;
    info->product_id = entry->info.productId;
    info->product_version = 0;
}

static const IHS_HIDProviderClass ProviderClass = {
        .alloc = ProviderAlloc,
        .free = ProviderFree,
        .supportsDevice = ProviderSupportsDevice,
        .openDevice = ProviderOpenDevice,
        .hasChange = ProviderHasChange,
        .enumerateDevices = ProviderEnumerate,
        .deviceInfo = ProviderDeviceInfo,
};

IHS_HIDProvider *TSL_GamepadProviderCreate(void) {
    return IHS_SessionHIDProviderCreate(&ProviderClass);
}

void TSL_GamepadProviderDestroy(IHS_HIDProvider *provider) {
    IHS_SessionHIDProviderDestroy(provider);
}

void TSL_GamepadProviderSetRumbleCallback(IHS_HIDProvider *provider, TSL_GamepadRumbleFunction *function,
                                          void *context) {
    GamepadProvider *pads = (GamepadProvider *) provider;
    IHS_MutexLock(pads->lock);
    pads->rumble = function;
    pads->rumbleContext = context;
    IHS_MutexUnlock(pads->lock);
}

bool TSL_GamepadProviderSetConnected(IHS_HIDProvider *provider, int slot, bool connected,
                                     const TSL_GamepadInfo *info) {
    if (slot < 0 || slot >= TSL_GAMEPAD_SLOTS) return false;
    GamepadProvider *pads = (GamepadProvider *) provider;
    IHS_MutexLock(pads->lock);
    Slot *s = &pads->slots[slot];
    bool changed = s->connected != connected;
    s->connected = connected;
    if (connected) {
        if (info != NULL) {
            s->info = *info;
            s->info.name[sizeof(s->info.name) - 1] = '\0';
        }
    } else {
        memset(&s->state, 0, sizeof(s->state));
    }
    IHS_MutexUnlock(pads->lock);
    return changed;
}

void TSL_GamepadProviderUpdate(IHS_HIDProvider *provider, int slot, const TSL_GamepadState *state) {
    if (slot < 0 || slot >= TSL_GAMEPAD_SLOTS) return;
    GamepadProvider *pads = (GamepadProvider *) provider;
    IHS_MutexLock(pads->lock);
    pads->slots[slot].state = *state;
    IHS_MutexUnlock(pads->lock);
}

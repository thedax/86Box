/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          RawInput joystick interface.
 *
 * Authors: Miran Grca, <mgrca8@gmail.com>
 *          GH Cao, <driver1998.ms@outlook.com>
 *          Jasmine Iwanek, <jriwanek@gmail.com>
 *
 *          Copyright 2016-2018 Miran Grca.
 *          Copyright 2020 GH Cao.
 *          Copyright 2021-2025 Jasmine Iwanek.
 */
#include <windows.h>
#include <windowsx.h>
#include <hidclass.h>
#include <hidusage.h>
#include <hidsdi.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/plat.h>
#include <86box/gameport.h>
#include <86box/win.h>

/* These are defined in hidusage.h in the Windows SDK, but not in mingw-w64. */
#ifndef HID_USAGE_SIMULATION_AILERON
#    define HID_USAGE_SIMULATION_AILERON ((USAGE) 0xb0)
#endif
#ifndef HID_USAGE_SIMULATION_ELEVATOR
#    define HID_USAGE_SIMULATION_ELEVATOR ((USAGE) 0xb8)
#endif
#ifndef HID_USAGE_SIMULATION_ACCELLERATOR
#    define HID_USAGE_SIMULATION_ACCELLERATOR ((USAGE) 0xc4)
#endif
#ifndef HID_USAGE_SIMULATION_BRAKE
#    define HID_USAGE_SIMULATION_BRAKE ((USAGE) 0xc5)
#endif
#ifndef HID_USAGE_SIMULATION_CLUTCH
#    define HID_USAGE_SIMULATION_CLUTCH ((USAGE) 0xc6)
#endif
#ifndef HID_USAGE_SIMULATION_SHIFTER
#    define HID_USAGE_SIMULATION_SHIFTER ((USAGE) 0xc7)
#endif
#ifndef HID_USAGE_SIMULATION_STEERING
#    define HID_USAGE_SIMULATION_STEERING ((USAGE) 0xc8)
#endif

#ifdef ENABLE_JOYSTICK_LOG
int joystick_do_log = ENABLE_JOYSTICK_LOG;

static void
joystick_log(const char *fmt, ...)
{
    va_list ap;

    if (joystick_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define joystick_log(fmt, ...)
#endif

typedef struct {
    HANDLE               hdevice;
    PHIDP_PREPARSED_DATA data;

    USAGE usage_button[256];

    struct raw_axis_t {
        USAGE  usage;
        USHORT link;
        USHORT bitsize;
        LONG   max;
        LONG   min;
    } axis[MAX_JOY_AXES];

    struct raw_pov_t {
        USAGE  usage;
        USHORT link;
        LONG   max;
        LONG   min;
    } pov[MAX_JOY_POVS];
} raw_joystick_t;

int                   joysticks_present = 0;
joystick_state_t      joystick_state[GAMEPORT_MAX][MAX_JOYSTICKS];
plat_joystick_state_t plat_joystick_state[MAX_PLAT_JOYSTICKS];

raw_joystick_t raw_joystick_state[MAX_PLAT_JOYSTICKS];

/* We only use the first 32 buttons reported, from Usage ID 1-128 */
void
joystick_add_button(raw_joystick_t *rawjoy, plat_joystick_state_t *joy, USAGE usage)
{
    if (joy->nr_buttons >= MAX_JOY_BUTTONS)
        return;
    if (usage < 1 || usage > 128)
        return;

    rawjoy->usage_button[usage] = joy->nr_buttons;
    sprintf(joy->button[joy->nr_buttons].name, "Button %d", usage);
    joy->nr_buttons++;
}

void
joystick_add_axis(raw_joystick_t *rawjoy, plat_joystick_state_t *joy, PHIDP_VALUE_CAPS prop)
{
    if (joy->nr_axes >= MAX_JOY_AXES)
        return;

    switch (prop->Range.UsageMin) {
        case HID_USAGE_GENERIC_X:
            sprintf(joy->axis[joy->nr_axes].name, "X");
            break;
        case HID_USAGE_GENERIC_Y:
            sprintf(joy->axis[joy->nr_axes].name, "Y");
            break;
        case HID_USAGE_GENERIC_Z:
            sprintf(joy->axis[joy->nr_axes].name, "Z");
            break;
        case HID_USAGE_GENERIC_RX:
            sprintf(joy->axis[joy->nr_axes].name, "RX");
            break;
        case HID_USAGE_GENERIC_RY:
            sprintf(joy->axis[joy->nr_axes].name, "RY");
            break;
        case HID_USAGE_GENERIC_RZ:
            sprintf(joy->axis[joy->nr_axes].name, "RZ");
            break;
        case HID_USAGE_GENERIC_SLIDER:
            sprintf(joy->axis[joy->nr_axes].name, "Slider");
            break;
        case HID_USAGE_GENERIC_DIAL:
            sprintf(joy->axis[joy->nr_axes].name, "Dial");
            break;
        case HID_USAGE_GENERIC_WHEEL:
            sprintf(joy->axis[joy->nr_axes].name, "Wheel");
            break;
        case HID_USAGE_SIMULATION_AILERON:
            sprintf(joy->axis[joy->nr_axes].name, "Aileron");
            break;
        case HID_USAGE_SIMULATION_ELEVATOR:
            sprintf(joy->axis[joy->nr_axes].name, "Elevator");
            break;
        case HID_USAGE_SIMULATION_RUDDER:
            sprintf(joy->axis[joy->nr_axes].name, "Rudder");
            break;
        case HID_USAGE_SIMULATION_THROTTLE:
            sprintf(joy->axis[joy->nr_axes].name, "Throttle");
            break;
        case HID_USAGE_SIMULATION_ACCELLERATOR:
            sprintf(joy->axis[joy->nr_axes].name, "Accelerator");
            break;
        case HID_USAGE_SIMULATION_BRAKE:
            sprintf(joy->axis[joy->nr_axes].name, "Brake");
            break;
        case HID_USAGE_SIMULATION_CLUTCH:
            sprintf(joy->axis[joy->nr_axes].name, "Clutch");
            break;
        case HID_USAGE_SIMULATION_SHIFTER:
            sprintf(joy->axis[joy->nr_axes].name, "Shifter");
            break;
        case HID_USAGE_SIMULATION_STEERING:
            sprintf(joy->axis[joy->nr_axes].name, "Steering");
            break;
        default:
            return;
    }

    joy->axis[joy->nr_axes].id         = joy->nr_axes;
    rawjoy->axis[joy->nr_axes].usage   = prop->Range.UsageMin;
    rawjoy->axis[joy->nr_axes].link    = prop->LinkCollection;
    rawjoy->axis[joy->nr_axes].bitsize = prop->BitSize;

    /* Assume unsigned when min >= 0 */
    if (prop->LogicalMin < 0) {
        rawjoy->axis[joy->nr_axes].max = prop->LogicalMax;
    } else {
        /*
         * Some joysticks will send -1 in LogicalMax, like Xbox Controllers
         * so we need to mask that to appropriate value (instead of 0xFFFFFFFF)
         */
        rawjoy->axis[joy->nr_axes].max = prop->LogicalMax & ((1ULL << prop->BitSize) - 1);
    }
    rawjoy->axis[joy->nr_axes].min = prop->LogicalMin;

    joy->nr_axes++;
}

void
joystick_add_pov(raw_joystick_t *rawjoy, plat_joystick_state_t *joy, PHIDP_VALUE_CAPS prop)
{
    if (joy->nr_povs >= MAX_JOY_POVS)
        return;

    sprintf(joy->pov[joy->nr_povs].name, "POV %d", joy->nr_povs + 1);
    rawjoy->pov[joy->nr_povs].usage = prop->Range.UsageMin;
    rawjoy->pov[joy->nr_povs].link  = prop->LinkCollection;
    rawjoy->pov[joy->nr_povs].min   = prop->LogicalMin;
    rawjoy->pov[joy->nr_povs].max   = prop->LogicalMax;

    joy->nr_povs++;
}

void
joystick_get_capabilities(raw_joystick_t *rawjoy, plat_joystick_state_t *joy)
{
    UINT              size     = 0;
    PHIDP_BUTTON_CAPS btn_caps = NULL;
    PHIDP_VALUE_CAPS  val_caps = NULL;
    HIDP_CAPS         caps;

    /* Get preparsed data (HID data format) */
    GetRawInputDeviceInfoW(rawjoy->hdevice, RIDI_PREPARSEDDATA, NULL, &size);
    rawjoy->data = malloc(size);
    if (GetRawInputDeviceInfoW(rawjoy->hdevice, RIDI_PREPARSEDDATA, rawjoy->data, &size) <= 0)
        fatal("joystick_get_capabilities: Failed to get preparsed data.\n");

    HidP_GetCaps(rawjoy->data, &caps);

    /* Buttons */
    if (caps.NumberInputButtonCaps > 0) {
        btn_caps = calloc(caps.NumberInputButtonCaps, sizeof(HIDP_BUTTON_CAPS));
        if (HidP_GetButtonCaps(HidP_Input, btn_caps, &caps.NumberInputButtonCaps, rawjoy->data) != HIDP_STATUS_SUCCESS) {
            joystick_log("joystick_get_capabilities: Failed to query input buttons.\n");
            goto end;
        }
        /* We only detect generic stuff */
        for (int c = 0; c < caps.NumberInputButtonCaps; c++) {
            if (btn_caps[c].UsagePage != HID_USAGE_PAGE_BUTTON)
                continue;

            int button_count = btn_caps[c].Range.UsageMax - btn_caps[c].Range.UsageMin + 1;
            for (int b = 0; b < button_count; b++) {
                joystick_add_button(rawjoy, joy, b + btn_caps[c].Range.UsageMin);
            }
        }
    }

    /* Values (axes and povs) */
    if (caps.NumberInputValueCaps > 0) {
        val_caps = calloc(caps.NumberInputValueCaps, sizeof(HIDP_VALUE_CAPS));
        if (HidP_GetValueCaps(HidP_Input, val_caps, &caps.NumberInputValueCaps, rawjoy->data) != HIDP_STATUS_SUCCESS) {
            joystick_log("joystick_get_capabilities: Failed to query axes and povs.\n");
            goto end;
        }
        /* We only detect generic stuff */
        for (int c = 0; c < caps.NumberInputValueCaps; c++) {
            if (val_caps[c].UsagePage != HID_USAGE_PAGE_GENERIC)
                continue;

            if (val_caps[c].Range.UsageMin == HID_USAGE_GENERIC_HATSWITCH)
                joystick_add_pov(rawjoy, joy, &val_caps[c]);
            else
                joystick_add_axis(rawjoy, joy, &val_caps[c]);
        }
    }

end:
    free(btn_caps);
    free(val_caps);
}

void
joystick_get_device_name(raw_joystick_t *rawjoy, plat_joystick_state_t *joy, PRID_DEVICE_INFO info)
{
    UINT   size                  = 0;
    WCHAR *device_name           = NULL;
    WCHAR  device_desc_wide[200] = { 0 };

    GetRawInputDeviceInfoW(rawjoy->hdevice, RIDI_DEVICENAME, device_name, &size);
    device_name = calloc(size, sizeof(WCHAR));
    if (GetRawInputDeviceInfoW(rawjoy->hdevice, RIDI_DEVICENAME, device_name, &size) <= 0)
        fatal("joystick_get_capabilities: Failed to get device name.\n");

    HANDLE hDevObj = CreateFileW(device_name, GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hDevObj) {
        HidD_GetProductString(hDevObj, device_desc_wide, sizeof(WCHAR) * 200);
        CloseHandle(hDevObj);
    }
    free(device_name);

    int result = WideCharToMultiByte(CP_ACP, 0, device_desc_wide, 200, joy->name, 260, NULL, NULL);
    if (result == 0 || strlen(joy->name) == 0)
        sprintf(joy->name,
                "RawInput %s, VID:%04lX PID:%04lX",
                info->hid.usUsage == HID_USAGE_GENERIC_JOYSTICK ? "Joystick" : "Gamepad",
                info->hid.dwVendorId,
                info->hid.dwProductId);
}

void
joystick_init(void)
{
    UINT size = 0;
    atexit(joystick_close);

    joysticks_present = 0;
    memset(raw_joystick_state, 0, sizeof(raw_joystick_t) * MAX_PLAT_JOYSTICKS);

    /* Get a list of raw input devices from Windows */
    UINT raw_devices = 0;
    GetRawInputDeviceList(NULL, &raw_devices, sizeof(RAWINPUTDEVICELIST));
    PRAWINPUTDEVICELIST deviceList = calloc(raw_devices, sizeof(RAWINPUTDEVICELIST));
    GetRawInputDeviceList(deviceList, &raw_devices, sizeof(RAWINPUTDEVICELIST));

    for (int i = 0; i < raw_devices; i++) {
        PRID_DEVICE_INFO info = NULL;

        if (joysticks_present >= MAX_PLAT_JOYSTICKS)
            break;
        if (deviceList[i].dwType != RIM_TYPEHID)
            continue;

        /* Get device info: hardware IDs and usage IDs */
        GetRawInputDeviceInfoA(deviceList[i].hDevice, RIDI_DEVICEINFO, NULL, &size);
        info         = malloc(size);
        info->cbSize = sizeof(RID_DEVICE_INFO);
        if (GetRawInputDeviceInfoA(deviceList[i].hDevice, RIDI_DEVICEINFO, info, &size) <= 0)
            goto end_loop;

        /* If this is not a joystick/gamepad, skip */
        if (info->hid.usUsagePage != HID_USAGE_PAGE_GENERIC)
            goto end_loop;
        if (info->hid.usUsage != HID_USAGE_GENERIC_JOYSTICK && info->hid.usUsage != HID_USAGE_GENERIC_GAMEPAD)
            goto end_loop;

        plat_joystick_state_t *joy    = &plat_joystick_state[joysticks_present];
        raw_joystick_t        *rawjoy = &raw_joystick_state[joysticks_present];
        rawjoy->hdevice               = deviceList[i].hDevice;

        joystick_get_capabilities(rawjoy, joy);
        joystick_get_device_name(rawjoy, joy, info);

        joystick_log("joystick_init: %s - %d buttons, %d axes, %d POVs\n",
                     joy->name, joy->nr_buttons, joy->nr_axes, joy->nr_povs);

        joysticks_present++;

end_loop:
        free(info);
    }

    free(deviceList);
    joystick_log("joystick_init: joysticks_present=%i\n", joysticks_present);

    /* Initialize the RawInput (joystick and gamepad) module. */
    RAWINPUTDEVICE ridev[2];
    ridev[0].dwFlags     = 0;
    ridev[0].hwndTarget  = NULL;
    ridev[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
    ridev[0].usUsage     = HID_USAGE_GENERIC_JOYSTICK;

    ridev[1].dwFlags     = 0;
    ridev[1].hwndTarget  = NULL;
    ridev[1].usUsagePage = HID_USAGE_PAGE_GENERIC;
    ridev[1].usUsage     = HID_USAGE_GENERIC_GAMEPAD;

    if (!RegisterRawInputDevices(ridev, 2, sizeof(RAWINPUTDEVICE)))
        fatal("plat_joystick_init: RegisterRawInputDevices failed\n");
}

void
joystick_close(void)
{
    RAWINPUTDEVICE ridev[2];
    ridev[0].dwFlags     = RIDEV_REMOVE;
    ridev[0].hwndTarget  = NULL;
    ridev[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
    ridev[0].usUsage     = HID_USAGE_GENERIC_JOYSTICK;

    ridev[1].dwFlags     = RIDEV_REMOVE;
    ridev[1].hwndTarget  = NULL;
    ridev[1].usUsagePage = HID_USAGE_PAGE_GENERIC;
    ridev[1].usUsage     = HID_USAGE_GENERIC_GAMEPAD;

    RegisterRawInputDevices(ridev, 2, sizeof(RAWINPUTDEVICE));
}

void
win_joystick_handle(PRAWINPUT raw)
{
    HRESULT r;
    int     j = -1; /* current joystick index, -1 when not found */

    /* If the input is not from a known device, we ignore it */
    for (int i = 0; i < joysticks_present; i++) {
        if (raw_joystick_state[i].hdevice == raw->header.hDevice) {
            j = i;
            break;
        }
    }
    if (j == -1)
        return;

    /* Read buttons */
    USAGE usage_list[128] = { 0 };
    ULONG usage_length    = plat_joystick_state[j].nr_buttons;
    memset(plat_joystick_state[j].b, 0, MAX_JOY_BUTTONS * sizeof(int));

    r = HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_BUTTON, 0, usage_list, &usage_length,
                       raw_joystick_state[j].data, (PCHAR) raw->data.hid.bRawData, raw->data.hid.dwSizeHid);

    if (r == HIDP_STATUS_SUCCESS) {
        for (int i = 0; i < usage_length; i++) {
            int button                          = raw_joystick_state[j].usage_button[usage_list[i]];
            plat_joystick_state[j].b[button] = 128;
        }
    }

    /* Read axes */
    for (int axis_nr = 0; axis_nr < plat_joystick_state[j].nr_axes; axis_nr++) {
        const struct raw_axis_t *axis   = &raw_joystick_state[j].axis[axis_nr];
        ULONG                    uvalue = 0;
        LONG                     value  = 0;
        LONG                     center = (axis->max - axis->min + 1) / 2;

        r = HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, axis->link, axis->usage, &uvalue,
                               raw_joystick_state[j].data, (PCHAR) raw->data.hid.bRawData, raw->data.hid.dwSizeHid);

        if (r == HIDP_STATUS_SUCCESS) {
            if (axis->min < 0) {
                /* extend signed uvalue to LONG */
                if (uvalue & (1 << (axis->bitsize - 1))) {
                    ULONG mask = (1 << axis->bitsize) - 1;
                    value      = -1U ^ mask;
                    value |= uvalue;
                } else {
                    value = uvalue;
                }
            } else {
                /* Assume unsigned when min >= 0, convert to a signed value */
                value = (LONG) uvalue - center;
            }
            if (abs(value) == 1)
                value = 0;
            value = value * 32768 / center;
        }

        plat_joystick_state[j].a[axis_nr] = value;
#if 0
        joystick_log("%s %-06d ", plat_joystick_state[0][j].axis[axis_nr].name, plat_joystick_state[j].a[axis_nr]);
#endif
    }

    /* read povs */
    for (int pov_nr = 0; pov_nr < plat_joystick_state[j].nr_povs; pov_nr++) {
        const struct raw_pov_t *pov    = &raw_joystick_state[j].pov[pov_nr];
        ULONG                   uvalue = 0;
        LONG                    value  = -1;

        r = HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, pov->link, pov->usage, &uvalue,
                               raw_joystick_state[j].data, (PCHAR) raw->data.hid.bRawData, raw->data.hid.dwSizeHid);

        if (r == HIDP_STATUS_SUCCESS && (uvalue >= pov->min && uvalue <= pov->max)) {
            value = (uvalue - pov->min) * 36000;
            value /= (pov->max - pov->min + 1);
            value %= 36000;
        }

        plat_joystick_state[j].p[pov_nr] = value;

#if 0
        joystick_log("%s %-3d ", plat_joystick_state[0][j].pov[pov_nr].name, plat_joystick_state[j].p[pov_nr]);
#endif
    }
#if 0
    joystick_log("\n");
#endif
}

static int
joystick_get_axis(int joystick_nr, int mapping)
{
    if (mapping & POV_X) {
        int pov = plat_joystick_state[joystick_nr].p[mapping & 3];
        if (LOWORD(pov) == 0xFFFF)
            return 0;
        else
            return sin((2 * M_PI * (double) pov) / 36000.0) * 32767;
    } else if (mapping & POV_Y) {
        int pov = plat_joystick_state[joystick_nr].p[mapping & 3];

        if (LOWORD(pov) == 0xFFFF)
            return 0;
        else
            return -cos((2 * M_PI * (double) pov) / 36000.0) * 32767;
    } else
        return plat_joystick_state[joystick_nr].a[plat_joystick_state[joystick_nr].axis[mapping].id];
}

void
joystick_process(void)
{
    if (joystick_type == JS_TYPE_NONE)
        return;

    for (int js = 0; js < joystick_get_max_joysticks(joystick_type); js++) {
        if (joystick_state[0][js].plat_joystick_nr) {
            int joystick_nr = joystick_state[0][js].plat_joystick_nr - 1;

            for (int axis_nr = 0; axis_nr < joystick_get_axis_count(joystick_type); axis_nr++)
                joystick_state[0][js].axis[axis_nr] = joystick_get_axis(joystick_nr, joystick_state[0][js].axis_mapping[axis_nr]);

            for (int button_nr = 0; button_nr < joystick_get_button_count(joystick_type); button_nr++)
                joystick_state[0][js].button[button_nr] = plat_joystick_state[joystick_nr].b[joystick_state[0][js].button_mapping[button_nr]];

            for (int pov_nr = 0; pov_nr < joystick_get_pov_count(joystick_type); pov_nr++) {
                int    x         = joystick_get_axis(joystick_nr, joystick_state[0][js].pov_mapping[pov_nr][0]);
                int    y         = joystick_get_axis(joystick_nr, joystick_state[0][js].pov_mapping[pov_nr][1]);
                double angle     = (atan2((double) y, (double) x) * 360.0) / (2 * M_PI);
                double magnitude = sqrt((double) x * (double) x + (double) y * (double) y);

                if (magnitude < 16384)
                    joystick_state[0][js].pov[pov_nr] = -1;
                else
                    joystick_state[0][js].pov[pov_nr] = ((int) angle + 90 + 360) % 360;
            }
        } else {
            for (int axis_nr = 0; axis_nr < joystick_get_axis_count(joystick_type); axis_nr++)
                joystick_state[0][js].axis[axis_nr] = 0;

            for (int button_nr = 0; button_nr < joystick_get_button_count(joystick_type); button_nr++)
                joystick_state[0][js].button[button_nr] = 0;

            for (int pov_nr = 0; pov_nr < joystick_get_pov_count(joystick_type); pov_nr++)
                joystick_state[0][js].pov[pov_nr] = -1;
        }
    }
}

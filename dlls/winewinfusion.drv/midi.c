#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "mmsystem.h"
#include "mmddk.h"

#include "wine/debug.h"
#include "wine/unixlib.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(winfusionmidi);

#define WINFUSION_CALL(func, params) \
    WINE_UNIX_CALL(winfusion_unix_##func, params)
#define WINFUSION_MAX_SYSEX_SIZE 1048576

enum midi_device_state
{
    MIDI_DEVICE_CLOSED,
    MIDI_DEVICE_OPENING,
    MIDI_DEVICE_OPEN,
    MIDI_DEVICE_CLOSING,
};

struct midi_device
{
    enum midi_device_state state;
    MIDIOPENDESC desc;
    UINT callback_flags;
    UINT generation;
    UINT long_messages;
    DWORD callback_thread;
    BYTE running_status;
};

struct midi_notification
{
    BOOL send;
    DWORD_PTR callback;
    UINT callback_flags;
    HDRVR device;
    DWORD_PTR instance;
    UINT message;
    DWORD_PTR param1;
    DWORD_PTR param2;
};

static SRWLOCK device_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE device_condition = CONDITION_VARIABLE_INIT;
static struct midi_device device;

static void set_notification(struct midi_notification *notify,
        const struct midi_device *source, UINT message,
        DWORD_PTR param1, DWORD_PTR param2)
{
    notify->send = TRUE;
    notify->callback = source->desc.dwCallback;
    notify->callback_flags = source->callback_flags;
    notify->device = (HDRVR)source->desc.hMidi;
    notify->instance = source->desc.dwInstance;
    notify->message = message;
    notify->param1 = param1;
    notify->param2 = param2;
}

static void send_notification(const struct midi_notification *notify)
{
    if (!notify->send) return;
    DriverCallback(notify->callback, notify->callback_flags, notify->device,
            notify->message, notify->instance, notify->param1, notify->param2);
}

static UINT midi_out_get_num_devs(void)
{
    return GetEnvironmentVariableA("WINFUSION_SHM", NULL, 0) ? 1 : 0;
}

static UINT midi_out_get_devcaps(UINT dev_id, MIDIOUTCAPSW *caps, UINT size)
{
    MIDIOUTCAPSW value;

    if (dev_id) return MMSYSERR_BADDEVICEID;
    if (!caps) return MMSYSERR_INVALPARAM;
    memset(&value, 0, sizeof(value));
    value.wMid = MM_MICROSOFT;
    value.wPid = 1;
    value.vDriverVersion = 0x0100;
    lstrcpynW(value.szPname, L"WinFusion MIDI Synth", ARRAY_SIZE(value.szPname));
    value.wTechnology = MOD_SWSYNTH;
    value.wVoices = 256;
    value.wNotes = 256;
    value.wChannelMask = 0xffff;
    memcpy(caps, &value, min(size, sizeof(value)));
    return MMSYSERR_NOERROR;
}

static UINT midi_out_open(UINT dev_id, MIDIOPENDESC *desc, UINT flags,
        struct midi_notification *notify)
{
    struct winfusion_result_params params;
    UINT generation;

    if (dev_id) return MMSYSERR_BADDEVICEID;
    if (!desc) return MMSYSERR_INVALPARAM;
    if (flags & ~CALLBACK_TYPEMASK) return MMSYSERR_INVALFLAG;

    AcquireSRWLockExclusive(&device_lock);
    if (device.state != MIDI_DEVICE_CLOSED)
    {
        ReleaseSRWLockExclusive(&device_lock);
        return MMSYSERR_ALLOCATED;
    }
    generation = ++device.generation;
    device.state = MIDI_DEVICE_OPENING;
    ReleaseSRWLockExclusive(&device_lock);

    WINFUSION_CALL(open, &params);

    AcquireSRWLockExclusive(&device_lock);
    if (device.state != MIDI_DEVICE_OPENING || device.generation != generation)
    {
        if (params.result == MMSYSERR_NOERROR) params.result = MMSYSERR_NODRIVER;
    }
    else if (params.result == MMSYSERR_NOERROR)
    {
        device.state = MIDI_DEVICE_OPEN;
        device.desc = *desc;
        device.callback_flags = HIWORD(flags & CALLBACK_TYPEMASK);
        device.running_status = 0;
        set_notification(notify, &device, MOM_OPEN, 0, 0);
    }
    else
    {
        device.state = MIDI_DEVICE_CLOSED;
    }
    ReleaseSRWLockExclusive(&device_lock);
    return params.result;
}

static UINT midi_out_close(UINT dev_id, struct midi_notification *notify)
{
    struct winfusion_result_params params;
    DWORD thread_id = GetCurrentThreadId();
    UINT generation;

    if (dev_id) return MMSYSERR_BADDEVICEID;
    AcquireSRWLockExclusive(&device_lock);
    while (device.state == MIDI_DEVICE_OPEN && device.long_messages &&
           device.callback_thread != thread_id)
        SleepConditionVariableSRW(&device_condition, &device_lock, INFINITE, 0);
    if (device.state != MIDI_DEVICE_OPEN)
    {
        ReleaseSRWLockExclusive(&device_lock);
        return MMSYSERR_ERROR;
    }
    generation = device.generation;
    device.state = MIDI_DEVICE_CLOSING;
    ReleaseSRWLockExclusive(&device_lock);

    WINFUSION_CALL(close, &params);

    AcquireSRWLockExclusive(&device_lock);
    if (device.state == MIDI_DEVICE_CLOSING && device.generation == generation)
    {
        set_notification(notify, &device, MOM_CLOSE, 0, 0);
        device.state = MIDI_DEVICE_CLOSED;
        memset(&device.desc, 0, sizeof(device.desc));
        device.callback_flags = 0;
        device.running_status = 0;
    }
    ReleaseSRWLockExclusive(&device_lock);
    return params.result;
}

static UINT midi_out_data(UINT dev_id, DWORD data)
{
    struct winfusion_short_params params;
    BYTE status = data;
    BYTE data1;
    BYTE data2;
    BYTE length;

    if (dev_id) return MMSYSERR_BADDEVICEID;
    AcquireSRWLockExclusive(&device_lock);
    if (device.state != MIDI_DEVICE_OPEN)
    {
        ReleaseSRWLockExclusive(&device_lock);
        return MIDIERR_NODEVICE;
    }

    if (status & 0x80)
    {
        data1 = data >> 8;
        data2 = data >> 16;
        if (status < 0xf0)
            device.running_status = status;
        else if (status <= 0xf7)
            device.running_status = 0;
    }
    else if (device.running_status)
    {
        status = device.running_status;
        data1 = data;
        data2 = data >> 8;
    }
    else
    {
        ReleaseSRWLockExclusive(&device_lock);
        return MMSYSERR_INVALPARAM;
    }

    if ((status & 0xf0) < 0x80 || (status & 0xf0) > 0xe0)
    {
        ReleaseSRWLockExclusive(&device_lock);
        return MMSYSERR_NOTSUPPORTED;
    }
    length = (status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0 ? 2 : 3;
    params.status = status;
    params.data1 = data1;
    params.data2 = data2;
    params.length = length;
    ReleaseSRWLockExclusive(&device_lock);

    WINFUSION_CALL(short, &params);
    return params.result;
}

static UINT midi_out_long_data(UINT dev_id, MIDIHDR *header, UINT header_size)
{
    struct midi_notification notify = {0};
    struct winfusion_sysex_params params;
    struct midi_device source;
    UINT generation;

    if (dev_id) return MMSYSERR_BADDEVICEID;
    if (header_size < offsetof(MIDIHDR, dwOffset) || !header || !header->lpData)
        return MMSYSERR_INVALPARAM;
    if (!(header->dwFlags & MHDR_PREPARED)) return MIDIERR_UNPREPARED;
    if (header->dwFlags & MHDR_INQUEUE) return MIDIERR_STILLPLAYING;
    if (header->dwBufferLength < 2 ||
        header->dwBufferLength > WINFUSION_MAX_SYSEX_SIZE ||
        (BYTE)header->lpData[0] != 0xf0 ||
        (BYTE)header->lpData[header->dwBufferLength - 1] != 0xf7)
        return MMSYSERR_INVALPARAM;

    AcquireSRWLockExclusive(&device_lock);
    if (device.state != MIDI_DEVICE_OPEN)
    {
        ReleaseSRWLockExclusive(&device_lock);
        return MIDIERR_NODEVICE;
    }
    if (device.long_messages)
    {
        ReleaseSRWLockExclusive(&device_lock);
        return MIDIERR_NOTREADY;
    }
    source = device;
    generation = device.generation;
    device.long_messages++;
    header->dwFlags &= ~MHDR_DONE;
    header->dwFlags |= MHDR_INQUEUE;
    ReleaseSRWLockExclusive(&device_lock);

    params.data = header->lpData;
    params.size = header->dwBufferLength;
    WINFUSION_CALL(sysex, &params);

    header->dwFlags &= ~MHDR_INQUEUE;
    if (params.result == MMSYSERR_NOERROR)
    {
        header->dwFlags |= MHDR_DONE;
        AcquireSRWLockExclusive(&device_lock);
        if (device.state == MIDI_DEVICE_OPEN && device.generation == generation)
            device.running_status = 0;
        device.callback_thread = GetCurrentThreadId();
        ReleaseSRWLockExclusive(&device_lock);
        set_notification(&notify, &source, MOM_DONE, (DWORD_PTR)header, 0);
        send_notification(&notify);
    }
    AcquireSRWLockExclusive(&device_lock);
    device.callback_thread = 0;
    device.long_messages--;
    WakeAllConditionVariable(&device_condition);
    ReleaseSRWLockExclusive(&device_lock);
    return params.result;
}

static UINT midi_out_prepare(UINT dev_id, MIDIHDR *header, UINT header_size)
{
    if (dev_id) return MMSYSERR_BADDEVICEID;
    if (header_size < offsetof(MIDIHDR, dwOffset) || !header || !header->lpData)
        return MMSYSERR_INVALPARAM;
    if (header->dwFlags & MHDR_PREPARED) return MMSYSERR_NOERROR;
    header->lpNext = NULL;
    header->dwFlags |= MHDR_PREPARED;
    header->dwFlags &= ~(MHDR_DONE | MHDR_INQUEUE);
    return MMSYSERR_NOERROR;
}

static UINT midi_out_unprepare(UINT dev_id, MIDIHDR *header, UINT header_size)
{
    if (dev_id) return MMSYSERR_BADDEVICEID;
    if (header_size < offsetof(MIDIHDR, dwOffset) || !header || !header->lpData)
        return MMSYSERR_INVALPARAM;
    if (!(header->dwFlags & MHDR_PREPARED)) return MMSYSERR_NOERROR;
    if (header->dwFlags & MHDR_INQUEUE) return MIDIERR_STILLPLAYING;
    header->dwFlags &= ~MHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

static UINT midi_out_reset(UINT dev_id)
{
    struct winfusion_result_params params;
    UINT generation;

    if (dev_id) return MMSYSERR_BADDEVICEID;
    AcquireSRWLockExclusive(&device_lock);
    if (device.state != MIDI_DEVICE_OPEN)
    {
        ReleaseSRWLockExclusive(&device_lock);
        return MIDIERR_NODEVICE;
    }
    generation = device.generation;
    ReleaseSRWLockExclusive(&device_lock);

    WINFUSION_CALL(reset, &params);

    if (params.result == MMSYSERR_NOERROR)
    {
        AcquireSRWLockExclusive(&device_lock);
        if (device.state == MIDI_DEVICE_OPEN && device.generation == generation)
            device.running_status = 0;
        ReleaseSRWLockExclusive(&device_lock);
    }
    return params.result;
}

static void midi_release(void)
{
    struct winfusion_result_params params;

    AcquireSRWLockExclusive(&device_lock);
    device.state = MIDI_DEVICE_CLOSED;
    device.generation++;
    memset(&device.desc, 0, sizeof(device.desc));
    device.callback_flags = 0;
    device.running_status = 0;
    ReleaseSRWLockExclusive(&device_lock);
    WINFUSION_CALL(release, &params);
}

DWORD WINAPI WINFUSION_modMessage(UINT dev_id, UINT message, DWORD_PTR user,
        DWORD_PTR param1, DWORD_PTR param2)
{
    struct midi_notification notify = {0};
    UINT result;

    (void)user;
    TRACE("dev %u message %#x user %Ix param1 %Ix param2 %Ix\n",
            dev_id, message, user, param1, param2);
    switch (message)
    {
    case DRVM_INIT:
    case DRVM_ENABLE:
    case DRVM_DISABLE:
        return MMSYSERR_NOERROR;
    case DRVM_EXIT:
        midi_release();
        return MMSYSERR_NOERROR;
    case MODM_GETNUMDEVS:
        return midi_out_get_num_devs();
    case MODM_GETDEVCAPS:
        return midi_out_get_devcaps(dev_id, (MIDIOUTCAPSW *)param1, param2);
    case MODM_OPEN:
        result = midi_out_open(dev_id, (MIDIOPENDESC *)param1, param2, &notify);
        break;
    case MODM_CLOSE:
        result = midi_out_close(dev_id, &notify);
        break;
    case MODM_DATA:
        return midi_out_data(dev_id, param1);
    case MODM_LONGDATA:
        return midi_out_long_data(dev_id, (MIDIHDR *)param1, param2);
    case MODM_PREPARE:
        return midi_out_prepare(dev_id, (MIDIHDR *)param1, param2);
    case MODM_UNPREPARE:
        return midi_out_unprepare(dev_id, (MIDIHDR *)param1, param2);
    case MODM_RESET:
        return midi_out_reset(dev_id);
    default:
        return MMSYSERR_NOTSUPPORTED;
    }
    send_notification(&notify);
    return result;
}

LRESULT CALLBACK WINFUSION_DriverProc(DWORD_PTR dev_id, HDRVR driver,
        UINT message, LPARAM param1, LPARAM param2)
{
    (void)dev_id;
    (void)driver;
    (void)param1;
    (void)param2;
    switch (message)
    {
    case DRV_LOAD:
    case DRV_OPEN:
    case DRV_CLOSE:
    case DRV_ENABLE:
    case DRV_DISABLE:
    case DRV_QUERYCONFIGURE:
    case DRV_CONFIGURE:
        return 1;
    case DRV_FREE:
        midi_release();
        return 1;
    case DRV_INSTALL:
    case DRV_REMOVE:
        return DRV_SUCCESS;
    default:
        return 0;
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        if (__wine_init_unix_call()) return FALSE;
    }
    return TRUE;
}

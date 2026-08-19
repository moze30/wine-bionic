#if 0
#pragma makedep unix
#endif

#include "config.h"

#ifndef __WINE_PE_BUILD

#include <pthread.h>
#include <stdint.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "mmsystem.h"
#include "wine/unixlib.h"

#include <shm/drivers/midi_driver.h>

#include "unixlib.h"

#define WINFUSION_MIDI_TIMEOUT UINT64_C(5000000000)

static pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t client_condition = PTHREAD_COND_INITIALIZER;
static MidiIPCClient *client;
static unsigned int client_references;
static int client_opening;
static int client_closing;

static UINT map_open_result(MidiIPCResult result)
{
    switch (result)
    {
    case MIDI_IPC_OK:
        return MMSYSERR_NOERROR;
    case MIDI_IPC_BUSY:
        return MMSYSERR_ALLOCATED;
    case MIDI_IPC_WOULD_BLOCK:
    case MIDI_IPC_TIMED_OUT:
        return MMSYSERR_NOTENABLED;
    case MIDI_IPC_INVALID_ARGUMENT:
        return MMSYSERR_INVALPARAM;
    case MIDI_IPC_DISCONNECTED:
    case MIDI_IPC_PROTOCOL_ERROR:
    case MIDI_IPC_SYSTEM_ERROR:
    case MIDI_IPC_SYNTH_ERROR:
    default:
        return MMSYSERR_NODRIVER;
    }
}

static UINT map_output_result(MidiIPCResult result)
{
    switch (result)
    {
    case MIDI_IPC_OK:
        return MMSYSERR_NOERROR;
    case MIDI_IPC_BUSY:
        return MMSYSERR_HANDLEBUSY;
    case MIDI_IPC_WOULD_BLOCK:
    case MIDI_IPC_TIMED_OUT:
        return MIDIERR_NOTREADY;
    case MIDI_IPC_DISCONNECTED:
        return MIDIERR_NODEVICE;
    case MIDI_IPC_INVALID_ARGUMENT:
        return MMSYSERR_INVALPARAM;
    case MIDI_IPC_PROTOCOL_ERROR:
    case MIDI_IPC_SYNTH_ERROR:
    case MIDI_IPC_SYSTEM_ERROR:
    default:
        return MMSYSERR_ERROR;
    }
}

static MidiIPCClient *client_acquire(void)
{
    MidiIPCClient *current = NULL;

    pthread_mutex_lock(&client_mutex);
    if (client && !client_closing)
    {
        current = client;
        client_references++;
    }
    pthread_mutex_unlock(&client_mutex);
    return current;
}

static void client_release_reference(void)
{
    pthread_mutex_lock(&client_mutex);
    if (!--client_references) pthread_cond_broadcast(&client_condition);
    pthread_mutex_unlock(&client_mutex);
}

static MidiIPCClient *client_begin_close(void)
{
    MidiIPCClient *old_client;

    pthread_mutex_lock(&client_mutex);
    while (client_opening || client_closing)
        pthread_cond_wait(&client_condition, &client_mutex);
    old_client = client;
    if (old_client)
    {
        client = NULL;
        client_closing = 1;
        while (client_references)
            pthread_cond_wait(&client_condition, &client_mutex);
    }
    pthread_mutex_unlock(&client_mutex);
    return old_client;
}

static void client_end_close(void)
{
    pthread_mutex_lock(&client_mutex);
    client_closing = 0;
    pthread_cond_broadcast(&client_condition);
    pthread_mutex_unlock(&client_mutex);
}

static NTSTATUS unix_open(void *args)
{
    struct winfusion_result_params *params = args;
    MidiIPCClient *new_client = NULL;
    MidiIPCResult result;

    pthread_mutex_lock(&client_mutex);
    if (client || client_opening || client_closing)
    {
        pthread_mutex_unlock(&client_mutex);
        params->result = MMSYSERR_ALLOCATED;
        return STATUS_SUCCESS;
    }
    client_opening = 1;
    pthread_mutex_unlock(&client_mutex);

    result = midi_ipc_client_connect_from_env(&new_client, WINFUSION_MIDI_TIMEOUT);
    params->result = map_open_result(result);

    pthread_mutex_lock(&client_mutex);
    if (result == MIDI_IPC_OK) client = new_client;
    client_opening = 0;
    pthread_cond_broadcast(&client_condition);
    pthread_mutex_unlock(&client_mutex);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_close(void *args)
{
    struct winfusion_result_params *params = args;
    MidiIPCClient *old_client = client_begin_close();

    if (old_client)
    {
        midi_ipc_client_close(old_client, WINFUSION_MIDI_TIMEOUT);
        midi_ipc_client_destroy(old_client);
        client_end_close();
    }
    params->result = MMSYSERR_NOERROR;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_short(void *args)
{
    struct winfusion_short_params *params = args;
    MidiIPCClient *current = client_acquire();

    if (!current)
        params->result = MIDIERR_NODEVICE;
    else
    {
        params->result = map_output_result(midi_ipc_client_send_short(
                current, params->status, params->data1, params->data2,
                params->length, WINFUSION_MIDI_TIMEOUT));
        client_release_reference();
    }
    return STATUS_SUCCESS;
}

static NTSTATUS unix_sysex(void *args)
{
    struct winfusion_sysex_params *params = args;
    MidiIPCClient *current = client_acquire();

    if (!current)
        params->result = MIDIERR_NODEVICE;
    else
    {
        params->result = map_output_result(midi_ipc_client_send_sysex(
                current, params->data, params->size, WINFUSION_MIDI_TIMEOUT));
        client_release_reference();
    }
    return STATUS_SUCCESS;
}

static NTSTATUS unix_reset(void *args)
{
    struct winfusion_result_params *params = args;
    MidiIPCClient *current = client_acquire();

    if (!current)
        params->result = MIDIERR_NODEVICE;
    else
    {
        params->result = map_output_result(midi_ipc_client_reset(
                current, WINFUSION_MIDI_TIMEOUT));
        client_release_reference();
    }
    return STATUS_SUCCESS;
}

static NTSTATUS unix_release(void *args)
{
    MidiIPCClient *old_client = client_begin_close();

    (void)args;
    if (old_client)
    {
        midi_ipc_client_destroy(old_client);
        client_end_close();
    }
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    unix_open,
    unix_close,
    unix_short,
    unix_sysex,
    unix_reset,
    unix_release,
};

C_ASSERT(ARRAY_SIZE(__wine_unix_call_funcs) == winfusion_unix_funcs_count);

#ifdef _WIN64

struct winfusion_sysex_params32
{
    UINT data;
    UINT size;
    UINT result;
};

static NTSTATUS wow64_unix_sysex(void *args)
{
    struct winfusion_sysex_params32 *params32 = args;
    struct winfusion_sysex_params params;

    params.data = (const void *)(uintptr_t)params32->data;
    params.size = params32->size;
    unix_sysex(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    unix_open,
    unix_close,
    unix_short,
    wow64_unix_sysex,
    unix_reset,
    unix_release,
};

C_ASSERT(ARRAY_SIZE(__wine_unix_call_wow64_funcs) == winfusion_unix_funcs_count);

#endif

#endif

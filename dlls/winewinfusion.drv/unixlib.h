#ifndef __WINE_WINEWINFUSION_UNIXLIB_H
#define __WINE_WINEWINFUSION_UNIXLIB_H

#include "windef.h"

struct winfusion_result_params
{
    UINT result;
};

struct winfusion_short_params
{
    BYTE status;
    BYTE data1;
    BYTE data2;
    BYTE length;
    UINT result;
};

struct winfusion_sysex_params
{
    const void *data;
    UINT size;
    UINT result;
};

enum winfusion_unix_funcs
{
    winfusion_unix_open,
    winfusion_unix_close,
    winfusion_unix_short,
    winfusion_unix_sysex,
    winfusion_unix_reset,
    winfusion_unix_release,
    winfusion_unix_funcs_count
};

#endif

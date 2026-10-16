/*
 *	wiiuse
 *
 *	Written By:
 *		Michael Laforest	< para >
 *		Email: < thepara (--AT--) g m a i l [--DOT--] com >
 *
 *	Copyright 2006-2007
 *
 *	This file is part of wiiuse.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 3 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *	$Header$
 *
 */

/**
 *	@file
 *	@brief Handles device I/O for Windows.
 */

#include <time.h>

#include "events.h"
#include "io.h"
#include "os.h"

#ifdef WIIUSE_WIN32
#include <stdlib.h>
#include <string.h> /* wiimosc patch: strcmp, strncpy */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <hidsdi.h>
#include <setupapi.h>

unsigned long wiiuse_os_ticks()
{
    FILETIME ft;
    ULARGE_INTEGER hnsTime;

#if _WIN32_WINNT >= _WIN32_WINNT_WIN10
    GetSystemTimePreciseAsFileTime(&ft);
#else
    GetSystemTimeAsFileTime(&ft);
#endif

    hnsTime.LowPart = ft.dwLowDateTime;
    hnsTime.HighPart = ft.dwHighDateTime;

    return hnsTime.QuadPart / 10000ULL;
}

/* wiimosc patch: wiiuse_os_find() can be called repeatedly to pick up devices that connect later.
 * Devices that are already open are skipped, and new ones go into slots that aren't connected. */
static int is_device_open(struct wiimote_t **wm, int max_wiimotes, const char *path)
{
    int i;
    for (i = 0; i < max_wiimotes; ++i)
    {
        if (WIIMOTE_IS_CONNECTED(wm[i]) && strcmp(wm[i]->dev_path, path) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static int next_free_slot(struct wiimote_t **wm, int max_wiimotes)
{
    int i;
    for (i = 0; i < max_wiimotes; ++i)
    {
        if (!WIIMOTE_IS_CONNECTED(wm[i]))
        {
            return i;
        }
    }
    return -1;
}

/* wiimosc patch: releases what a previous device left in the slot, and resets it to defaults */
static void reset_slot(struct wiimote_t *wm)
{
    if (wm->dev_handle)
    {
        CloseHandle(wm->dev_handle);
    }
    if (wm->hid_overlap.hEvent)
    {
        CloseHandle(wm->hid_overlap.hEvent);
    }
    wiiuse_init_wiimote(wm, wm->unid);
}

int wiiuse_os_find(struct wiimote_t **wm, int max_wiimotes, int timeout)
{
    GUID device_id;
    HANDLE dev;
    HDEVINFO device_info;
    int i, index, slot;
    DWORD len;
    SP_DEVICE_INTERFACE_DATA device_data;
    PSP_DEVICE_INTERFACE_DETAIL_DATA detail_data = NULL;
    HIDD_ATTRIBUTES attr;
    int found = 0;

    (void)timeout; /* unused */

    device_data.cbSize = sizeof(device_data);
    index              = 0;

    /* get the device id */
    HidD_GetHidGuid(&device_id);

    /* get all hid devices connected */
    device_info = SetupDiGetClassDevs(&device_id, NULL, NULL, (DIGCF_DEVICEINTERFACE | DIGCF_PRESENT));

    for (;; ++index)
    {

        if (detail_data)
        {
            free(detail_data);
            detail_data = NULL;
        }

        /* query the next hid device info */
        if (!SetupDiEnumDeviceInterfaces(device_info, NULL, &device_id, index, &device_data))
        {
            break;
        }

        /* get the size of the data block required */
        i                   = SetupDiGetDeviceInterfaceDetail(device_info, &device_data, NULL, 0, &len, NULL);
        detail_data         = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)malloc(len);
        detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        /* query the data for this device */
        if (!SetupDiGetDeviceInterfaceDetail(device_info, &device_data, detail_data, len, NULL, NULL))
        {
            continue;
        }

        /* wiimosc patch: skip devices that are already open */
        if (is_device_open(wm, max_wiimotes, detail_data->DevicePath))
        {
            continue;
        }

        /* open the device */
        dev = CreateFile(detail_data->DevicePath, (GENERIC_READ | GENERIC_WRITE),
                         (FILE_SHARE_READ | FILE_SHARE_WRITE), NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                         NULL);
        if (dev == INVALID_HANDLE_VALUE)
        {
            continue;
        }

        /* get device attributes */
        attr.Size = sizeof(attr);
        i         = HidD_GetAttributes(dev, &attr);

        if ((attr.VendorID == WM_VENDOR_ID)
            && ((attr.ProductID == WM_PRODUCT_ID) || (attr.ProductID == WM_PRODUCT_ID_TR)))
        {
            /* this is a wiimote */

            /* wiimosc patch: fill the next free slot instead of counting up from 0 */
            slot = next_free_slot(wm, max_wiimotes);
            if (slot < 0)
            {
                CloseHandle(dev);
                break;
            }
            reset_slot(wm[slot]);
            strncpy(wm[slot]->dev_path, detail_data->DevicePath, sizeof(wm[slot]->dev_path) - 1);

            wm[slot]->dev_handle = dev;

            if (attr.ProductID == WM_PRODUCT_ID_TR)
                wm[slot]->type = WIIUSE_WIIMOTE_MOTION_PLUS_INSIDE;

            wm[slot]->hid_overlap.hEvent     = CreateEvent(NULL, 1, 1, "");
            wm[slot]->hid_overlap.Offset     = 0;
            wm[slot]->hid_overlap.OffsetHigh = 0;

            WIIMOTE_ENABLE_STATE(wm[slot], WIIMOTE_STATE_DEV_FOUND);
            WIIMOTE_ENABLE_STATE(wm[slot], WIIMOTE_STATE_CONNECTED);

            /* try to set the output report to see if the device is actually connected */
            if (!wiiuse_set_report_type(wm[slot]))
            {
                /* wiimosc patch: release the device, so the slot can be reused */
                reset_slot(wm[slot]);
                continue;
            }

            /* do the handshake */
            wiiuse_handshake(wm[slot], NULL, 0);

            WIIUSE_INFO("Connected to wiimote [id %i].", wm[slot]->unid);

            ++found;
        } else
        {
            /* not a wiimote */
            CloseHandle(dev);
        }
    }

    if (detail_data)
    {
        free(detail_data);
    }

    SetupDiDestroyDeviceInfoList(device_info);

    return found;
}

int wiiuse_os_connect(struct wiimote_t **wm, int wiimotes)
{
    int connected = 0;
    int i         = 0;

    for (; i < wiimotes; ++i)
    {
        if (!wm[i])
        {
            continue;
        }
        if (WIIMOTE_IS_SET(wm[i], WIIMOTE_STATE_CONNECTED))
        {
            ++connected;
        }
    }

    return connected;
}

void wiiuse_os_disconnect(struct wiimote_t *wm)
{
    if (!wm || WIIMOTE_IS_CONNECTED(wm))
    {
        return;
    }

    CloseHandle(wm->dev_handle);
    wm->dev_handle = 0;

    ResetEvent(&wm->hid_overlap);

    wm->event = WIIUSE_NONE;

    WIIMOTE_DISABLE_STATE(wm, WIIMOTE_STATE_CONNECTED);
    WIIMOTE_DISABLE_STATE(wm, WIIMOTE_STATE_HANDSHAKE);
}

int wiiuse_os_poll(struct wiimote_t **wm, int wiimotes)
{
    int i;
    byte read_buffer[MAX_PAYLOAD];
    int evnt = 0;

    if (!wm)
    {
        return 0;
    }

    for (i = 0; i < wiimotes; ++i)
    {
        wm[i]->event = WIIUSE_NONE;

        /* clear out the buffer */
        memset(read_buffer, 0, sizeof(read_buffer));
        /* read */
        if (wiiuse_os_read(wm[i], read_buffer, sizeof(read_buffer)))
        {
            /* propagate the event */
            propagate_event(wm[i], read_buffer[0], read_buffer + 1);
            evnt += (wm[i]->event != WIIUSE_NONE);
        } else
        {
            /* send out any waiting writes */
            wiiuse_send_next_pending_write_request(wm[i]);
            idle_cycle(wm[i]);
        }
    }

    return evnt;
}

int wiiuse_os_read(struct wiimote_t *wm, byte *buf, int len)
{
    DWORD b, r;

    if (!wm || !WIIMOTE_IS_CONNECTED(wm))
    {
        return 0;
    }

    if (!ReadFile(wm->dev_handle, buf, len, &b, &wm->hid_overlap))
    {
        /* partial read */
        b = GetLastError();

        if ((b == ERROR_HANDLE_EOF) || (b == ERROR_DEVICE_NOT_CONNECTED))
        {
            /* remote disconnect */
            wiiuse_disconnected(wm);
            return 0;
        }
        r = WaitForSingleObject(wm->hid_overlap.hEvent, wm->timeout);
        if (r == WAIT_TIMEOUT)
        {
            /* timeout - cancel and continue */

            /*
                        if (*buf) {
                                WIIUSE_WARNING("Packet ignored.  This may indicate a problem (timeout is %i
               ms).", wm->timeout);
                        }
            */

            CancelIo(wm->dev_handle);
            /* wiimosc patch: CancelIo() doesn't wait for the cancellation. Without waiting, a late completion
             * writes into the caller's (stack) buffer after it is gone. */
            GetOverlappedResult(wm->dev_handle, &wm->hid_overlap, &b, TRUE);
            ResetEvent(wm->hid_overlap.hEvent);
            return 0;
        } else if (r == WAIT_FAILED)
        {
            WIIUSE_WARNING("A wait error occurred on reading from wiimote %i.", wm->unid);
            return 0;
        }

        if (!GetOverlappedResult(wm->dev_handle, &wm->hid_overlap, &b, 0))
        {
            return 0;
        }

/* log the received data */
#ifdef WITH_WIIUSE_DEBUG
        {
            DWORD i;
            printf("[DEBUG] (id %i) RECV: (%.2x) ", wm->unid, buf[0]);
            for (i = 1; i < b; i++)
            {
                printf("%.2x ", buf[i]);
            }
            printf("\n");
        }
#endif
    }

    ResetEvent(wm->hid_overlap.hEvent);
    return 1;
}

/* wiimosc patch: writes used to go through wm->hid_overlap without waiting for completion. A pending write's
 * completion then signalled the next read, which returned without data (making synchronous reads time out), and
 * the write buffer on the stack was still in use after returning. Each write now gets its own OVERLAPPED, and
 * waits for completion. */
#define WIIMOSC_WRITE_TIMEOUT_MS 500

static BOOL write_file_sync(struct wiimote_t *wm, byte *data, DWORD len)
{
    OVERLAPPED overlap;
    DWORD bytes = 0;
    BOOL ok;

    memset(&overlap, 0, sizeof(overlap));
    overlap.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!overlap.hEvent)
    {
        return FALSE;
    }

    ok = WriteFile(wm->dev_handle, data, len, &bytes, &overlap);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
    {
        if (WaitForSingleObject(overlap.hEvent, WIIMOSC_WRITE_TIMEOUT_MS) != WAIT_OBJECT_0)
        {
            CancelIoEx(wm->dev_handle, &overlap);
        }
        ok = GetOverlappedResult(wm->dev_handle, &overlap, &bytes, TRUE);
    }

    CloseHandle(overlap.hEvent);
    return ok;
}

int wiiuse_os_write(struct wiimote_t *wm, byte report_type, byte *buf, int len)
{
    int i;
    byte write_buffer[MAX_PAYLOAD];

    if (!wm || !WIIMOTE_IS_CONNECTED(wm))
    {
        return 0;
    }

    write_buffer[0] = report_type;
    memcpy(write_buffer + 1, buf, len);

    switch (wm->stack)
    {
    case WIIUSE_STACK_UNKNOWN:
    {
        /* try to auto-detect the stack type */
        if (i = write_file_sync(wm, write_buffer, 22))
        {
            /* bluesoleil will always return 1 here, even if it's not connected */
            wm->stack = WIIUSE_STACK_BLUESOLEIL;
            return i;
        }

        if (i = HidD_SetOutputReport(wm->dev_handle, write_buffer, len + 1))
        {
            wm->stack = WIIUSE_STACK_MS;
            return i;
        }

        WIIUSE_ERROR("Unable to determine bluetooth stack type.");
        return 0;
    }

  /* Windows should always use WriteFile instead of HidD_SetOutputReport to communicate -> data pipe instead of control pipe*/
    case WIIUSE_STACK_MS:
        return write_file_sync(wm, write_buffer, 22);

    case WIIUSE_STACK_BLUESOLEIL:
        return write_file_sync(wm, write_buffer, 22);
    }

    return 0;
}

void wiiuse_init_platform_fields(struct wiimote_t *wm)
{
    wm->dev_handle     = 0;
    wm->stack          = WIIUSE_STACK_UNKNOWN;
    wm->normal_timeout = WIIMOTE_DEFAULT_TIMEOUT;
    wm->exp_timeout    = WIIMOTE_EXP_TIMEOUT;
    wm->timeout        = wm->normal_timeout;
}

void wiiuse_cleanup_platform_fields(struct wiimote_t *wm) { wm->dev_handle = 0; }

#endif /* ifdef WIIUSE_WIN32 */

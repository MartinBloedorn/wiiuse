/*
 *	wiiuse
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
 */

/**
 *	@file
 *	@brief Handles device I/O for macOS through IOKit HID (wiimosc patch).
 *
 *	Replaces the IOBluetooth backend (os_mac/), which runs its own inquiry and opens L2CAP channels directly; that
 *	no longer works on recent macOS versions, and can't pick up remotes the OS has paired. Like os_win.c, this
 *	backend only opens remotes that are already connected as HID devices (paired through the OS), and
 *	wiiuse_os_find() can be called repeatedly: open devices are skipped, new ones go into unconnected slots.
 *
 *	Input reports arrive on a private dispatch queue per device and are queued, so reads work from any thread
 *	(the hub opens devices on one thread and polls them on another) without a run loop.
 */

#include "events.h"
#include "io.h"
#include "os.h"

#ifdef WIIUSE_MAC

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDKeys.h>

#define WIIMOSC_READ_TIMEOUT_MS 10 /* as WIIMOTE_DEFAULT_TIMEOUT on Windows: paces the hub's poll loop */
#define WIIMOSC_CANCEL_TIMEOUT_MS 1000
#define WIIMOSC_QUEUE_SIZE 64
#define WIIMOSC_INPUT_BUFFER_SIZE 64

/* Input reports received on the device's dispatch queue, waiting to be read */
struct report_queue
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    byte reports[WIIMOSC_QUEUE_SIZE][MAX_PAYLOAD];
    int lengths[WIIMOSC_QUEUE_SIZE];
    int head;
    int count;
    int is_removed;

    dispatch_queue_t queue;
    dispatch_semaphore_t cancelled;
    int is_active;
    byte input_buffer[WIIMOSC_INPUT_BUFFER_SIZE]; /* filled by IOKit before each input report callback */
};

unsigned long wiiuse_os_ticks()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000);
}

static void on_input_report(void *context, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id,
                            uint8_t *report, CFIndex length)
{
    struct report_queue *q = (struct report_queue *)context;
    int index;
    (void)result;
    (void)sender;
    (void)type;
    (void)report_id;

    if (length <= 0)
    {
        return;
    }
    if (length > MAX_PAYLOAD)
    {
        length = MAX_PAYLOAD;
    }

    pthread_mutex_lock(&q->mutex);
    if (q->count == WIIMOSC_QUEUE_SIZE)
    {
        /* nobody reads: drop the oldest */
        q->head = (q->head + 1) % WIIMOSC_QUEUE_SIZE;
        --q->count;
    }
    /* the report starts with its id, which is what wiiuse expects in buf[0] */
    index = (q->head + q->count) % WIIMOSC_QUEUE_SIZE;
    memcpy(q->reports[index], report, (size_t)length);
    q->lengths[index] = (int)length;
    ++q->count;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

static void on_removed(void *context, IOReturn result, void *sender)
{
    struct report_queue *q = (struct report_queue *)context;
    (void)result;
    (void)sender;

    pthread_mutex_lock(&q->mutex);
    q->is_removed = 1;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

static int copy_string_property(IOHIDDeviceRef device, CFStringRef key, char *out, size_t size)
{
    CFTypeRef value = IOHIDDeviceGetProperty(device, key);
    if (!value || CFGetTypeID(value) != CFStringGetTypeID())
    {
        return 0;
    }
    return CFStringGetCString((CFStringRef)value, out, (CFIndex)size, kCFStringEncodingUTF8);
}

static int int_property(IOHIDDeviceRef device, CFStringRef key)
{
    int out          = 0;
    CFTypeRef value  = IOHIDDeviceGetProperty(device, key);
    if (value && CFGetTypeID(value) == CFNumberGetTypeID())
    {
        CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, &out);
    }
    return out;
}

/* Stops the callbacks, closes the device and frees the queue */
static void release_device(struct wiimote_t *wm)
{
    IOHIDDeviceRef device  = (IOHIDDeviceRef)wm->hid_device;
    struct report_queue *q = (struct report_queue *)wm->hid_reports;

    if (device)
    {
        if (q && q->is_active)
        {
            /* no callbacks run after the cancel handler, so the queue can be freed after it */
            IOHIDDeviceCancel(device);
            dispatch_semaphore_wait(q->cancelled,
                                    dispatch_time(DISPATCH_TIME_NOW, WIIMOSC_CANCEL_TIMEOUT_MS * NSEC_PER_MSEC));
        }
        IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
        CFRelease(device);
    }
    if (q)
    {
        if (q->queue)
        {
            dispatch_release(q->queue);
        }
        if (q->cancelled)
        {
            dispatch_release(q->cancelled);
        }
        pthread_cond_destroy(&q->cond);
        pthread_mutex_destroy(&q->mutex);
        free(q);
    }
    wm->hid_device  = NULL;
    wm->hid_reports = NULL;
}

/* Opens \a device for the slot, with input reports going into a new queue */
static int open_device(struct wiimote_t *wm, IOHIDDeviceRef device)
{
    struct report_queue *q;
    dispatch_semaphore_t cancelled;

    if (IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone) != kIOReturnSuccess)
    {
        WIIUSE_WARNING("Unable to open HID device %s.", wm->dev_path);
        return 0;
    }

    q = (struct report_queue *)calloc(1, sizeof(struct report_queue));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    q->queue     = dispatch_queue_create("wiiuse.hid", DISPATCH_QUEUE_SERIAL);
    q->cancelled = dispatch_semaphore_create(0);
    CFRetain(device);
    wm->hid_device  = (void *)device;
    wm->hid_reports = q;

    cancelled = q->cancelled;
    IOHIDDeviceSetDispatchQueue(device, q->queue);
    IOHIDDeviceSetCancelHandler(device, ^{
      dispatch_semaphore_signal(cancelled);
    });
    IOHIDDeviceRegisterInputReportCallback(device, q->input_buffer, sizeof(q->input_buffer), on_input_report, q);
    IOHIDDeviceRegisterRemovalCallback(device, on_removed, q);
    IOHIDDeviceActivate(device);
    q->is_active = 1;
    return 1;
}

static int is_device_open(struct wiimote_t **wm, int max_wiimotes, const char *path, const char *serial)
{
    int i;
    for (i = 0; i < max_wiimotes; ++i)
    {
        if (!WIIMOTE_IS_CONNECTED(wm[i]))
        {
            continue;
        }
        /* a remote may show up as more than one HID node: the serial (its address) identifies it, too */
        if (strcmp(wm[i]->dev_path, path) == 0 || (serial[0] && strcmp(wm[i]->dev_serial, serial) == 0))
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

/* releases what a previous device left in the slot, and resets it to defaults */
static void reset_slot(struct wiimote_t *wm)
{
    release_device(wm);
    wiiuse_init_wiimote(wm, wm->unid);
}

int wiiuse_os_find(struct wiimote_t **wm, int max_wiimotes, int timeout)
{
    CFMutableDictionaryRef matching;
    CFNumberRef vendor;
    io_iterator_t iterator;
    io_service_t service;
    int vendor_id = WM_VENDOR_ID;
    int found     = 0;

    (void)timeout; /* unused */

    matching = IOServiceMatching(kIOHIDDeviceKey);
    vendor   = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendor_id);
    CFDictionarySetValue(matching, CFSTR(kIOHIDVendorIDKey), vendor);
    CFRelease(vendor);
    /* consumes the matching dictionary */
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, matching, &iterator) != KERN_SUCCESS)
    {
        return 0;
    }

    while ((service = IOIteratorNext(iterator)))
    {
        IOHIDDeviceRef device;
        uint64_t entry_id = 0;
        char path[sizeof(wm[0]->dev_path)];
        char serial[sizeof(wm[0]->dev_serial)] = "";
        int product_id, slot;

        IORegistryEntryGetRegistryEntryID(service, &entry_id);
        snprintf(path, sizeof(path), "IOService:%llx", (unsigned long long)entry_id);
        device = IOHIDDeviceCreate(kCFAllocatorDefault, service);
        IOObjectRelease(service);
        if (!device)
        {
            continue;
        }

        product_id = int_property(device, CFSTR(kIOHIDProductIDKey));
        if (product_id != WM_PRODUCT_ID && product_id != WM_PRODUCT_ID_TR)
        {
            CFRelease(device);
            continue;
        }
        /* Bluetooth HID devices report their address as serial number, e.g. "8c-cd-e8-c4-d5-0f" */
        if (!copy_string_property(device, CFSTR(kIOHIDSerialNumberKey), serial, sizeof(serial)))
        {
            serial[0] = '\0';
        }
        if (is_device_open(wm, max_wiimotes, path, serial))
        {
            CFRelease(device);
            continue;
        }

        slot = next_free_slot(wm, max_wiimotes);
        if (slot < 0)
        {
            CFRelease(device);
            break;
        }
        reset_slot(wm[slot]);
        strncpy(wm[slot]->dev_path, path, sizeof(wm[slot]->dev_path) - 1);
        strncpy(wm[slot]->dev_serial, serial, sizeof(wm[slot]->dev_serial) - 1);

        if (!open_device(wm[slot], device))
        {
            CFRelease(device);
            reset_slot(wm[slot]);
            continue;
        }
        CFRelease(device); /* the slot holds its own reference */

        if (product_id == WM_PRODUCT_ID_TR)
        {
            wm[slot]->type = WIIUSE_WIIMOTE_MOTION_PLUS_INSIDE;
        }

        WIIMOTE_ENABLE_STATE(wm[slot], WIIMOTE_STATE_DEV_FOUND);
        WIIMOTE_ENABLE_STATE(wm[slot], WIIMOTE_STATE_CONNECTED);

        /* try to set the output report to see if the device is actually connected */
        if (!wiiuse_set_report_type(wm[slot]))
        {
            reset_slot(wm[slot]);
            continue;
        }

        /* do the handshake */
        wiiuse_handshake(wm[slot], NULL, 0);

        WIIUSE_INFO("Connected to wiimote [id %i].", wm[slot]->unid);

        ++found;
    }
    IOObjectRelease(iterator);

    return found;
}

int wiiuse_os_connect(struct wiimote_t **wm, int wiimotes)
{
    int connected = 0;
    int i;

    for (i = 0; i < wiimotes; ++i)
    {
        if (wm[i] && WIIMOTE_IS_SET(wm[i], WIIMOTE_STATE_CONNECTED))
        {
            ++connected;
        }
    }

    return connected;
}

void wiiuse_os_disconnect(struct wiimote_t *wm)
{
    if (!wm)
    {
        return;
    }

    /* Unlike os_win.c, this also closes the device while it's connected (as wiiuse_cleanup() expects) */
    release_device(wm);

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
    struct report_queue *q;
    struct timespec timeout;
    int length, is_removed;

    if (!wm || !WIIMOTE_IS_CONNECTED(wm) || !wm->hid_reports)
    {
        return 0;
    }
    q = (struct report_queue *)wm->hid_reports;

    timeout.tv_sec  = 0;
    timeout.tv_nsec = WIIMOSC_READ_TIMEOUT_MS * 1000000L;

    pthread_mutex_lock(&q->mutex);
    if (q->count == 0 && !q->is_removed)
    {
        pthread_cond_timedwait_relative_np(&q->cond, &q->mutex, &timeout);
    }
    is_removed = q->is_removed && q->count == 0;
    length     = 0;
    if (q->count > 0)
    {
        length = q->lengths[q->head] < len ? q->lengths[q->head] : len;
        memcpy(buf, q->reports[q->head], (size_t)length);
        q->head = (q->head + 1) % WIIMOSC_QUEUE_SIZE;
        --q->count;
    }
    pthread_mutex_unlock(&q->mutex);

    if (is_removed)
    {
        /* remote disconnect */
        wiiuse_disconnected(wm);
        return 0;
    }
    if (length == 0)
    {
        return 0;
    }

/* log the received data */
#ifdef WITH_WIIUSE_DEBUG
    {
        int i;
        printf("[DEBUG] (id %i) RECV: (%.2x) ", wm->unid, buf[0]);
        for (i = 1; i < length; i++)
        {
            printf("%.2x ", buf[i]);
        }
        printf("\n");
    }
#endif

    return 1;
}

int wiiuse_os_write(struct wiimote_t *wm, byte report_type, byte *buf, int len)
{
    byte write_buffer[MAX_PAYLOAD];
    IOReturn result;

    if (!wm || !WIIMOTE_IS_CONNECTED(wm) || !wm->hid_device || len + 1 > MAX_PAYLOAD)
    {
        return 0;
    }

    /* IOKit wants the report id as the first byte, as on Windows */
    write_buffer[0] = report_type;
    memcpy(write_buffer + 1, buf, len);

    /* sent as an output report, as hidapi (used by Dolphin on macOS) does; synchronous */
    result = IOHIDDeviceSetReport((IOHIDDeviceRef)wm->hid_device, kIOHIDReportTypeOutput, report_type,
                                  write_buffer, len + 1);
    if (result != kIOReturnSuccess)
    {
        WIIUSE_DEBUG("Writing report 0x%x failed (0x%x) [id %i].", report_type, result, wm->unid);
        return 0;
    }
    return len + 1;
}

void wiiuse_init_platform_fields(struct wiimote_t *wm)
{
    wm->objc_wm     = NULL;
    wm->hid_device  = NULL;
    wm->hid_reports = NULL;
}

void wiiuse_cleanup_platform_fields(struct wiimote_t *wm) { release_device(wm); }

#endif /* ifdef WIIUSE_MAC */

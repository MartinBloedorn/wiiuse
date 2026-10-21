/*
 *	wiiuse
 *
 *	Based on os_nix.c.
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
 *	@brief Handles device I/O for Linux over plain L2CAP sockets (wiimosc patch).
 *
 *	Replaces os_nix.c, which needs libbluetooth, and runs its own inquiry (a privileged HCI socket). This backend
 *	declares the few kernel structures it needs, and leaves discovery to the application: it passes the addresses
 *	of remotes in range to wiiuse_linux_offer(), and wiiuse_os_find() connects to them. Like os_win.c and
 *	os_mac_hid.c, wiiuse_os_find() can be called repeatedly: open devices are skipped, new ones go into
 *	unconnected slots.
 *
 *	The remotes aren't paired: a Wiimote accepts L2CAP connections without authentication, in 1+2 and SYNC mode
 *	alike. A remote paired with bluetoothd would be taken over by the kernel's hid-wiimote driver, which clashes
 *	with wiiuse's own handshake (it resets the report mode and detects extensions on every status report).
 */

#include "events.h"
#include "io.h"
#include "os.h"

#ifdef WIIUSE_LINUX_L2CAP

#include <endian.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define WIIMOSC_BTPROTO_L2CAP 0

/* struct sockaddr_l2 of <bluetooth/l2cap.h>; the address is stored in reverse byte order */
struct l2cap_address
{
    sa_family_t family;
    unsigned short psm;
    unsigned char address[6];
    unsigned short cid;
    unsigned char address_type;
};

#define WIIMOSC_READ_TIMEOUT_MS 10 /* as WIIMOTE_DEFAULT_TIMEOUT on Windows: paces the hub's poll loop */
#define WIIMOSC_CONNECT_TIMEOUT_MS 5000
#define WIIMOSC_MAX_OFFERS 8

struct offer
{
    char address[18];
    int is_motion_plus_inside;
};

static struct offer offers[WIIMOSC_MAX_OFFERS];
static int offer_count               = 0;
static pthread_mutex_t offers_mutex = PTHREAD_MUTEX_INITIALIZER;

unsigned long wiiuse_os_ticks()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000);
}

void wiiuse_linux_offer(const char *address, int is_motion_plus_inside)
{
    int i;

    pthread_mutex_lock(&offers_mutex);
    for (i = 0; i < offer_count; ++i)
    {
        if (strcmp(offers[i].address, address) == 0)
        {
            break;
        }
    }
    if (i == offer_count && offer_count < WIIMOSC_MAX_OFFERS)
    {
        snprintf(offers[i].address, sizeof(offers[i].address), "%s", address);
        offers[i].is_motion_plus_inside = is_motion_plus_inside;
        ++offer_count;
    }
    pthread_mutex_unlock(&offers_mutex);
}

static int take_offer(struct offer *out, struct wiimote_t **wm, int max_wiimotes)
{
    int j, found = 0;

    pthread_mutex_lock(&offers_mutex);
    while (!found && offer_count > 0)
    {
        *out = offers[0];
        memmove(offers, offers + 1, (size_t)(offer_count - 1) * sizeof(offers[0]));
        --offer_count;

        /* skip remotes that are connected already */
        found = 1;
        for (j = 0; j < max_wiimotes; ++j)
        {
            if (WIIMOTE_IS_CONNECTED(wm[j]) && strcmp(wm[j]->dev_serial, out->address) == 0)
            {
                found = 0;
                break;
            }
        }
    }
    pthread_mutex_unlock(&offers_mutex);
    return found;
}

static int parse_address(const char *text, unsigned char *out)
{
    unsigned int b[6];
    int i;

    if (sscanf(text, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
    {
        return 0;
    }
    for (i = 0; i < 6; ++i)
    {
        out[i] = (unsigned char)b[5 - i];
    }
    return 1;
}

/* Connects an L2CAP channel; returns the socket, or -1 */
static int connect_channel(const unsigned char *address, unsigned short psm, int *error)
{
    struct l2cap_address target;
    struct pollfd pfd;
    int fd, flags, result;

    fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, WIIMOSC_BTPROTO_L2CAP);
    if (fd < 0)
    {
        *error = errno;
        return -1;
    }

    memset(&target, 0, sizeof(target));
    target.family = AF_BLUETOOTH;
    target.psm    = htole16(psm);
    memcpy(target.address, address, 6);
    result = connect(fd, (struct sockaddr *)&target, sizeof(target));
    if (result < 0 && errno == EINPROGRESS)
    {
        pfd.fd     = fd;
        pfd.events = POLLOUT;
        result     = poll(&pfd, 1, WIIMOSC_CONNECT_TIMEOUT_MS);
        if (result == 0)
        {
            errno  = ETIMEDOUT;
            result = -1;
        } else if (result > 0)
        {
            socklen_t length = sizeof(flags);
            flags            = 0;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &flags, &length);
            errno  = flags;
            result = flags ? -1 : 0;
        }
    }
    if (result < 0)
    {
        *error = errno;
        close(fd);
        return -1;
    }
    return fd;
}

static void close_sockets(struct wiimote_t *wm)
{
    if (wm->in_sock >= 0)
    {
        close(wm->in_sock);
        wm->in_sock = -1;
    }
    if (wm->out_sock >= 0)
    {
        close(wm->out_sock);
        wm->out_sock = -1;
    }
}

/* releases what a previous device left in the slot, and resets it to defaults */
static void reset_slot(struct wiimote_t *wm)
{
    close_sockets(wm);
    wiiuse_init_wiimote(wm, wm->unid);
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

int wiiuse_os_find(struct wiimote_t **wm, int max_wiimotes, int timeout)
{
    struct offer candidate;
    int found = 0;

    (void)timeout; /* unused: nothing is scanned here */

    while (next_free_slot(wm, max_wiimotes) >= 0 && take_offer(&candidate, wm, max_wiimotes))
    {
        unsigned char address[6];
        int slot = next_free_slot(wm, max_wiimotes);
        int error;
        int out_sock, in_sock;

        if (!parse_address(candidate.address, address))
        {
            continue;
        }

        /* the output channel first, then the input channel, as the Wiimote expects */
        out_sock = connect_channel(address, WM_OUTPUT_CHANNEL, &error);
        in_sock  = out_sock < 0 ? -1 : connect_channel(address, WM_INPUT_CHANNEL, &error);
        if (in_sock < 0)
        {
            WIIUSE_WARNING("Unable to connect to %s: %s.", candidate.address, strerror(error));
            if (out_sock >= 0)
            {
                close(out_sock);
            }
            continue;
        }

        reset_slot(wm[slot]);
        wm[slot]->out_sock = out_sock;
        wm[slot]->in_sock  = in_sock;
        snprintf(wm[slot]->dev_path, sizeof(wm[slot]->dev_path), "%s", candidate.address);
        snprintf(wm[slot]->dev_serial, sizeof(wm[slot]->dev_serial), "%s", candidate.address);
        if (candidate.is_motion_plus_inside)
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
        break; /* the hub passes a single free slot per call anyway */
    }

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
    close_sockets(wm);

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
    struct pollfd pfd;
    int result, length;

    if (!wm || !WIIMOTE_IS_CONNECTED(wm) || wm->in_sock < 0)
    {
        return 0;
    }

    pfd.fd     = wm->in_sock;
    pfd.events = POLLIN;
    result     = poll(&pfd, 1, WIIMOSC_READ_TIMEOUT_MS);
    if (result < 0 && errno != EINTR)
    {
        wiiuse_disconnected(wm);
        return 0;
    }
    if (result <= 0)
    {
        return 0;
    }

    length = (int)recv(wm->in_sock, buf, (size_t)len, MSG_DONTWAIT);
    if (length <= 0)
    {
        /* 0 and errors other than "try again" mean the remote is gone (powered off, out of range) */
        if (length == 0 || (errno != EAGAIN && errno != EINTR))
        {
            wiiuse_disconnected(wm);
        }
        return 0;
    }

    /* the first byte is the HID header (0xa1, input); wiiuse expects the report id in buf[0] */
    memmove(buf, buf + 1, (size_t)(length - 1));

/* log the received data */
#ifdef WITH_WIIUSE_DEBUG
    {
        int i;
        printf("[DEBUG] (id %i) RECV: (%.2x) ", wm->unid, buf[0]);
        for (i = 1; i < length - 1; i++)
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
    int result;

    if (!wm || !WIIMOTE_IS_CONNECTED(wm) || wm->in_sock < 0 || len + 2 > MAX_PAYLOAD)
    {
        return 0;
    }

    write_buffer[0] = WM_SET_DATA | WM_BT_OUTPUT; /* HID header: output report */
    write_buffer[1] = report_type;
    memcpy(write_buffer + 2, buf, (size_t)len);

    result = (int)send(wm->in_sock, write_buffer, (size_t)len + 2, MSG_NOSIGNAL);
    if (result < 0)
    {
        WIIUSE_DEBUG("Writing report 0x%x failed (%s) [id %i].", report_type, strerror(errno), wm->unid);
        if (errno != EAGAIN && errno != EINTR)
        {
            wiiuse_disconnected(wm);
        }
        return 0;
    }
    return result;
}

void wiiuse_init_platform_fields(struct wiimote_t *wm)
{
    wm->out_sock = -1;
    wm->in_sock  = -1;
}

void wiiuse_cleanup_platform_fields(struct wiimote_t *wm) { close_sockets(wm); }

#endif /* ifdef WIIUSE_LINUX_L2CAP */

/*
 *  VBAN Receiver
 *
 *  Copyright (C) 2017 Raman Shyshniou <rommer@ibuffed.com>
 *  All Rights Reserved.
 *
 *  This is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This software is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this software; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 *  USA.
 */


#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>

#include "output.h"
#include "logger.h"
#include "streams.h"


static int64_t outpos;
static char *presence = NULL;
static char *buffer = NULL;
static long lost_total = 0;
static long cache; // frames
static int fd;


static void report_lost(long lost)
{
    lost_total += lost;

    if (lost == 1)
        logger(LOG_INF, "<out> lost 1 frame");
    else
        logger(LOG_INF, "<out> lost %ld frames", lost);
}


int output_init(char *pipename, struct stream *stream)
{
    char filename[PATH_MAX];
    char *s = pipename;
    char *d = filename;
    size_t l;

    for (; *s && d - filename < PATH_MAX - 1; s++) {
        switch (*s) {
            case '%':
                l = filename + PATH_MAX - d;
                switch (*(++s)) {
                    case '%':
                        *(d++) = *s;
                        break;
                    case 'f':
                        d += snprintf(d, l, "%s", stream->format_name);
                        break;
                    case 'r':
                        d += snprintf(d, l, "%ld", stream->sample_rate);
                        break;
                    case 'c':
                        d += snprintf(d, l, "%ld", stream->channels);
                        break;
                    default:
                        *(d++) = '%';
                        s--;
                }
                break;
            default:
                *(d++) = *s;
        }
    }

    *d = '\0';

    if ((fd = open(filename, O_WRONLY | O_NONBLOCK | O_CLOEXEC)) < 0)
        return -1;

    cache = stream->frames * BUFFER_OUT_PACKETS;

    return 0;
}


int output_done(void)
{
    if (close(fd))
        return -1;

    if (buffer)
        free(buffer);

    if (presence)
        free(presence);

    buffer = NULL;
    presence = NULL;
    lost_total = 0;

    return 0;
}


void output_play(int64_t ts, const char *data, long frames, long frame_size)
{
    long size, lost, off, len, i;

    size = frames * frame_size;

    assert(frames <= cache);

    if (!buffer) {
        buffer = malloc(cache * frame_size);
        if (!buffer)
            return;
    }

    if (!presence) {
        presence = malloc(cache);
        if (!presence)
            return;

        memcpy(buffer, data, size);
        memset(presence, 1, frames);
        if (frames < cache)
            bzero(presence + frames, cache - frames);
        outpos = ts;
    }

    if (ts <= outpos) {
        if (ts + frames <= outpos)
            // nothing to play in this packet
            return;

        off = (long) (outpos - ts);
        len = frames - off;

        memcpy(buffer, data + off * frame_size, len * frame_size);
        memset(presence, 1, len);

        return;
    }

    if (ts + frames <= outpos + cache) {
        off = (long) (ts - outpos);

        memcpy(buffer + off * frame_size, data, size);
        memset(presence + off, 1, frames);

        return;
    }

    len = (long) ((ts - outpos) + (int64_t) (frames - cache));
    outpos += len;

    // play frames from the start of buffer
    lost = 0;
    while (len) {
        if (*presence) {
            // calc length of block to play
            for (i = 1; i < len && i < cache && presence[i]; i++);

            if (write(fd, buffer, i * frame_size) < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // report overrun can be very noisy if source suspended
                    logger(LOG_DBG, "output overrun: %ld frames", i);
                } else {
                    logger(LOG_ERR, "write failed: %s", strerror(errno));
                    exit(1);
                }
            }

            if (i < cache) {
                memmove(buffer, buffer + i * frame_size, (cache - i) * frame_size);
                memmove(presence, presence + i, cache - i);
                bzero(presence + cache - i, i);
            } else {
                bzero(presence, cache);
            }
            len -= i;
        } else {
            // calc length of lost block
            for (i = 1; i < len && i < cache && !presence[i]; i++);

            if (i < cache) {
                memmove(buffer, buffer + i * frame_size, (cache - i) * frame_size);
                memmove(presence, presence + i, cache - i);
                bzero(presence + cache - i, i);
                report_lost(i);
                len -= i;
            } else {
                // lost whole cache
                lost = len;
                len = 0;
            }
        }
    }

    if (lost)
        report_lost(lost);

    off = (long) (ts - outpos);
    memcpy(buffer + off * frame_size, data, size);
    memset(presence + off, 1, frames);
}


void output_move(int64_t offset)
{
    outpos += offset;
}


long output_lost(void)
{
    return lost_total;
}

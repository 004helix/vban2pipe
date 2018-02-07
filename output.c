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


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

#include "output.h"
#include "logger.h"


static int64_t outpos;
static long cache = 512; // samples
static char *presence = NULL;
static char *buffer = NULL;
static long lost_total;


static void report_lost(long lost)
{
    lost_total += lost;

    if (lost == 1)
        logger(LOG_INF, "<out> lost 1 sample");
    else
        logger(LOG_INF, "<out> lost %ld samples", lost);
}


long output_lost(void)
{
    return lost_total;
}


void output_init(long cache_size)
{
    if (buffer)
        free(buffer);

    if (presence)
        free(presence);

    buffer = NULL;
    presence = NULL;
    lost_total = 0;
    cache = cache_size;
}


void output_move(int64_t offset)
{
    outpos += offset;
}


void output_play(int fd, int64_t ts, long samples,
                 const char *data, long size)
{
    long ss = size / samples;
    long lost, off, len, i;

    assert(samples <= cache);

    if (!buffer) {
        buffer = malloc(cache * ss);
        if (!buffer)
            return;
    }

    if (!presence) {
        presence = malloc(cache);
        if (!presence)
            return;

        memcpy(buffer, data, size);
        memset(presence, 1, samples);
        if (samples < cache)
            bzero(presence + samples, cache - samples);
        outpos = ts;
    }

    if (ts <= outpos) {
        if (ts + samples <= outpos)
            // nothing to play in this packet
            return;

        off = (long) (outpos - ts);
        len = samples - off;

        memcpy(buffer, data + off * ss, len * ss);
        memset(presence, 1, len);

        return;
    }

    if (ts + samples <= outpos + cache) {
        off = (long) (ts - outpos);

        memcpy(buffer + off * ss, data, size);
        memset(presence + off, 1, samples);

        return;
    }

    len = (long) ((ts - outpos) + (int64_t) (samples - cache));
    outpos += len;

    // play samples from the start of buffer
    lost = 0;
    while (len) {
        if (*presence) {
            // calc length of block to play
            for (i = 1; i < len && i < cache && presence[i]; i++);

            if (write(fd, buffer, i * ss) < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // report overrun can be very noisy if source suspended
                    logger(LOG_DBG, "output overrun: %ld samples", i);
                } else {
                    logger(LOG_ERR, "write failed: %s", strerror(errno));
                    exit(1);
                }
            }

            if (i < cache) {
                memmove(buffer, buffer + i * ss, (cache - i) * ss);
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
                memmove(buffer, buffer + i * ss, (cache - i) * ss);
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
    memcpy(buffer + off * ss, data, size);
    memset(presence + off, 1, samples);
}

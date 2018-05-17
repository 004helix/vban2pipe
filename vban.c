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

#define _DEFAULT_SOURCE
#include <endian.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include "vban.h"


static long sample_rates[] = {
    6000, 12000, 24000, 48000, 96000, 192000, 384000,
    8000, 16000, 32000, 64000, 128000, 256000, 512000,
    11025, 22050, 44100, 88200, 176400, 352800, 705600
};


static struct {
    char *name;  /* format name */
    long ss;     /* bytes per sample */
} formats[8] = {
    { "u8", 1 },
    { "s16le", 2 },
    { "s24le", 3 },
    { "s32le", 4 },
    { "float32le", 4 },
    { "float64le", 8 },
    { "unknown12", 0 },
    { "unknown10", 0 }
};


int vban_parse(const void *buffer, ssize_t size, struct vbaninfo *info)
{
    const unsigned char *header = buffer;
    int sridx;

    bzero(info, sizeof(struct vbaninfo));

    if (size < VBAN_HEADER_SIZE)
        return -1;

    if (strncmp((char *) buffer, "VBAN", 4))
        return -1;

    info->protocol = header[4] & 0xE0;

    sridx = header[4] & 0x1F;
    if (sridx >= sizeof(sample_rates) / sizeof(long))
        return -1;

    info->sample_rate = sample_rates[sridx];
    info->frames = 1L + (long) header[5];
    info->channels = 1L + (long) header[6];
    info->format = header[7] & 0x07;
    info->codec = header[7] & 0xF0;

    memcpy(info->stream_name, header + 8, 16);

    memcpy(&info->seq, header + 24, 4);
    info->seq = le32toh(info->seq);

    info->format_name = formats[info->format].name;
    info->sample_size = formats[info->format].ss;
    info->frame_size = info->sample_size * info->channels;

    if (size - VBAN_HEADER_SIZE != info->frames * info->frame_size)
        return -1;

    return 0;
}

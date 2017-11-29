/*
 *  Pulseaudio VBAN Receiver
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

#ifndef _VBAN_H
#define _VBAN_H 1

#include <stdint.h>
#include <unistd.h>

struct vbaninfo {
    uint32_t seq;      // Packet sequence number
    long protocol;     // VBAN Protocol
    long samples;      // Total samples in packet
    long sample_rate;  // Sample rate
    long sample_size;  // Bytes per sample
    long channels;     // Channels count
    long codec;        // Audio codec
    long datatype;     // Sample format
    char *dtname;      // Sample format name
    char name[20];     // Stream name
};

/*
 * VBAN protocol:
 * https://www.vb-audio.com/Voicemeeter/VBANProtocol_Specifications.pdf
 */

#define VBAN_HEADER_SIZE 28

/* VBAN protocols */
#define VBAN_PROTOCOL_AUDIO       0x00
#define VBAN_PROTOCOL_SERIAL      0x20
#define VBAN_PROTOCOL_TXT         0x40
#define VBAN_PROTOCOL_SERVICE     0x60
#define VBAN_PROTOCOL_UNDEFINED_1 0x80
#define VBAN_PROTOCOL_UNDEFINED_2 0xA0
#define VBAN_PROTOCOL_UNDEFINED_3 0xC0
#define VBAN_PROTOCOL_USER        0xE0

/* VBAN sample rate */
#define VBAN_SR_MAXNUMBER         21
extern long vban_srlist[VBAN_SR_MAXNUMBER];

/* VBAN datatype (sample format) */
#define VBAN_DATATYPE_BYTE8       0x00
#define VBAN_DATATYPE_INT16       0x01
#define VBAN_DATATYPE_INT24       0x02
#define VBAN_DATATYPE_INT32       0x03
#define VBAN_DATATYPE_FLOAT32     0x04
#define VBAN_DATATYPE_FLOAT64     0x05
#define VBAN_DATATYPE_12BITS      0x06
#define VBAN_DATATYPE_10BITS      0x07
struct vban_datatype {
    char *name;                   /* format name */
    long bps;                     /* bytes per sample */
};
extern struct vban_datatype vban_dtlist[8];

/* VBAN codecs */
#define VBAN_CODEC_PCM            0x00
#define VBAN_CODEC_VBCA           0x10 //VB-AUDIO AOIP CODEC
#define VBAN_CODEC_VBCV           0x20 //VB-AUDIO VOIP CODEC
#define VBAN_CODEC_UNDEFINED_1    0x30
#define VBAN_CODEC_UNDEFINED_2    0x40
#define VBAN_CODEC_UNDEFINED_3    0x50
#define VBAN_CODEC_UNDEFINED_4    0x60
#define VBAN_CODEC_UNDEFINED_5    0x70
#define VBAN_CODEC_UNDEFINED_6    0x80
#define VBAN_CODEC_UNDEFINED_7    0x90
#define VBAN_CODEC_UNDEFINED_8    0xA0
#define VBAN_CODEC_UNDEFINED_9    0xB0
#define VBAN_CODEC_UNDEFINED_10   0xC0
#define VBAN_CODEC_UNDEFINED_11   0xD0
#define VBAN_CODEC_UNDEFINED_12   0xE0
#define VBAN_CODEC_USER           0xF0

extern int vban_parse(const void *buffer, ssize_t size, struct vbaninfo *info);

#endif /* vban.h */

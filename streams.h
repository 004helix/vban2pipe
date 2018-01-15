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

#ifndef _STREAMS_H
#define _STREAMS_H 1

#include <time.h>
#include <stdint.h>
#include <net/if.h>
#include <sys/socket.h>
#include "vban.h"

/*
 * Streams
 */

struct packet {
    char *data; // packet data
    int sent; // sent to output
};

struct stream {
    // remote address
    struct sockaddr_storage peer;

    // receiving interface
    char ifname[IF_NAMESIZE];
    unsigned ifindex;

    // stream name
    char name[20];

    // data format
    long samples;           // samples per packet
    long sample_size;       // sample size in bytes
    long sample_rate;       // sample rate
    long channels;          // channels
    long datasize;          // total bytes in one packet
    long datatype;          // stream format (VBAN)
    char *dtname;           // sample format name

    // stream counters
    long lost;              // total lost packets counter
    uint32_t expected;      // next expected packet number in this stream
    struct packet curr;     // current packet in stream
    struct packet prev;     // previous packet in stream
    struct timespec ts;     // last packet received time

    // synchronization
    long ignore;            // ignore this stream
    long insync;            // synchronized with primary stream
    int64_t offset;         // stream offset

    // next stream
    struct stream *next;
};

extern struct stream *streams;

void forgetstreams(void);
void forgetstream(struct stream *);
struct stream *getstream(struct vbaninfo *, struct sockaddr *, unsigned ifindex);
struct stream *recvvban(int);

#endif

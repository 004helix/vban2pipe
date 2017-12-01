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

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>

#include "vban.h"
#include "streams.h"

#define DATA_BUFFER_SIZE 1436

struct stream *streams = NULL;


/*
 * Cleanup streams
 */
void forgetstreams(void)
{
    struct stream *stream = streams;

    while (stream) {
        struct stream *del = stream;
        stream = stream->next;

        fprintf(stderr, "[%s] stream offline\n", del->name);

        if (del->curr)
            free(del->curr);

        if (del->prev)
            free(del->prev);

        free(del);
    }

    streams = NULL;
}


/*
 * Remove stream
 */
void forgetstream(struct stream *stream)
{
    fprintf(stderr, "[%s] stream offline\n", stream->name);

    if (stream == streams) {
        streams = stream->next;
    } else {
        struct stream *prev;
        for (prev = streams; prev->next != stream; prev = prev->next);
        prev->next = stream->next;
    }

    if (stream->curr)
        free(stream->curr);

    if (stream->prev)
        free(stream->prev);

    free(stream);
}


/*
 * Find stream for the packet
 */
struct stream *getstream(struct vbaninfo *info, struct sockaddr *addr)
{
    struct stream *stream;

    if (!streams)
        return NULL;

    for (stream = streams; stream; stream = stream->next) {
        struct sockaddr *peer = (void *) &stream->peer;

        // check peer address family
        if (peer->sa_family != addr->sa_family)
            continue;

        // check peer address
        switch (peer->sa_family) {
            case AF_INET: {
                struct sockaddr_in *in1 = (void *) addr, *in2 = (void *) peer;
                if (in1->sin_port != in2->sin_port)
                    continue;
                if (in1->sin_addr.s_addr != in2->sin_addr.s_addr)
                    continue;
                break;
            }
#ifdef AF_INET6
            case AF_INET6: {
                struct sockaddr_in6 *in1 = (void *) addr, *in2 = (void *) peer;
                if (in1->sin6_port != in2->sin6_port)
                    continue;
                if (in1->sin6_flowinfo != in2->sin6_flowinfo)
                    continue;
                if (in1->sin6_scope_id != in2->sin6_scope_id)
                    continue;
                if (memcmp(in1->sin6_addr.s6_addr,
                           in2->sin6_addr.s6_addr,
                           sizeof(in1->sin6_addr.s6_addr)))
                    continue;
                break;
            }
#endif
            default:
                continue;
        }

        // check stream name
        if (strcmp(info->name, stream->name))
            continue;

        // stream found
        return stream;
    }

    return NULL;
}


/*
 * Receive and parse next VBAN packet
 */
struct stream *recvvban(int sock)
{
    char *buffer;
    char vban_header[VBAN_HEADER_SIZE];
    uint8_t aux[1024];
    int64_t delta;
    int64_t delta1;
    int64_t delta2;
    struct stream *stream;
    struct sockaddr_storage addr;
    struct timespec tspec;
    struct vbaninfo info;
    struct iovec iov[2];
    struct cmsghdr *cm;
    struct msghdr m;
    ssize_t size;
    int found_ts;

    buffer = malloc(DATA_BUFFER_SIZE);
    if (!buffer) {
        fprintf(stderr, "cannot allocate memory!\n");
        return NULL;
    }

    while (1) {
        iov[0].iov_base = vban_header;
        iov[0].iov_len = sizeof(vban_header);
        iov[1].iov_base = buffer;
        iov[1].iov_len = DATA_BUFFER_SIZE;

        m.msg_name = &addr;
        m.msg_namelen = sizeof(addr);
        m.msg_iov = iov;
        m.msg_iovlen = 2;
        m.msg_control = aux;
        m.msg_controllen = sizeof(aux);
        m.msg_flags = 0;

        size = recvmsg(sock, &m, 0);

        if (size < 0 && errno == EINTR)
            continue;

        if (size < 0 && errno == EAGAIN)
            return NULL;

        if (size < 0) {
            fprintf(stderr, "recvmsg: %s\n", strerror(errno));
            free(buffer);
            return NULL;
        }

        if (vban_parse(vban_header, size, &info) < 0) {
            fprintf(stderr, "malformed VBAN packet received\n");
            continue;
        }

        if (info.protocol != VBAN_PROTOCOL_AUDIO) {
            fprintf(stderr, "[%s] unsupporded protocol\n", info.name);
            continue;
        }

        if (info.codec != VBAN_CODEC_PCM) {
            fprintf(stderr, "[%s] unsupported audio codec\n", info.name);
            continue;
        }

        stream = getstream(&info, (struct sockaddr *) &addr);
        size -= VBAN_HEADER_SIZE;

        if (stream) {
            // check packet size
            if (size < stream->datasize) {
                fprintf(stderr, "[%s] too short packet received\n", info.name);
                continue;
            } else
            if (size > stream->datasize) {
                fprintf(stderr, "[%s] too long packet received\n", info.name);
                continue;
            }

            // check packet type
            if (stream->samples != info.samples ||
                stream->datatype != info.datatype ||
                stream->channels != info.channels ||
                stream->sample_rate != info.sample_rate) {
                fprintf(stderr, "[%s] bad packet received\n", info.name);
                continue;
            }
        } else {
            char peer[128];

            // parse peer address
            switch (((struct sockaddr *) &addr)->sa_family) {
                case AF_INET: {
                    struct sockaddr_in *in = (void *) &addr;
                    inet_ntop(AF_INET, &in->sin_addr, peer, sizeof(peer));
                    sprintf(peer + strlen(peer), ":%d", ntohs(in->sin_port));
                    break;
                };
#ifdef AF_INET6
                case AF_INET6: {
                    struct sockaddr_in6 *in6 = (void *) &addr;
                    peer[0] = '[';
                    inet_ntop(AF_INET6, &in6->sin6_addr, peer + 1, sizeof(peer) - 1);
                    sprintf(peer + strlen(peer), "]:%d", ntohs(in6->sin6_port));
                    break;
                };
#endif
                default:
                    fprintf(stderr, "[%s] unsupported address family\n", info.name);
                    continue;
            }

            // check data size
            if (size != info.samples * info.sample_size * info.channels) {
                fprintf(stderr, "[%s] invalid packet size\n", info.name);
                continue;
            }

            // create new stream entry
            stream = malloc(sizeof(struct stream));
            if (!stream) {
                fprintf(stderr, "[%s] cannot allocate memory\n", info.name);
                free(buffer);
                return NULL;
            }

            memcpy(&stream->peer, &addr, sizeof(struct sockaddr_storage));
            strcpy(stream->name, info.name);

            stream->samples = info.samples;
            stream->sample_size = info.sample_size;
            stream->sample_rate = info.sample_rate;
            stream->channels = info.channels;
            stream->datasize = size;
            stream->datatype = info.datatype;
            stream->dtname = info.dtname;

            stream->expected = info.seq;
            stream->curr = NULL;
            stream->prev = NULL;

            stream->ignore = 0;
            stream->insync = 0;
            stream->offset = 0;

            stream->next = NULL;

            fprintf(stderr, "[%s] stream connected from %s, %s, %ld Hz, %ld channel(s)\n",
                    stream->name, peer, stream->dtname, stream->sample_rate, stream->channels);

            if (streams) {
                struct stream *tail;
                for (tail = streams; tail->next; tail = tail->next);
                tail->next = stream;
            } else {
                streams = stream;
            }
        }

        // save timestamp
        found_ts = 0;
        for (cm = CMSG_FIRSTHDR(&m); cm; cm = CMSG_NXTHDR(&m, cm))
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_TIMESTAMPNS) {
                memcpy(&tspec, CMSG_DATA(cm), sizeof(struct timespec));
                found_ts++;
            }

        if (!found_ts) {
            fprintf(stderr, "[%s] couldn't find SCM_TIMESTAMPNS data in auxiliary recvmsg() data!\n",
                    stream->name);
            free(buffer);
            return NULL;
        }

        // save data to stream buffers
        if (stream->expected == info.seq) {
            if (stream->prev)
                free(stream->prev);

            stream->expected++;
            stream->tsprev = stream->tscurr;
            stream->tscurr = tspec;
            stream->prev = stream->curr;
            stream->curr = buffer;
            return stream;
        }

        // out of order packet received

        // check sequence overflow
        delta = (int64_t) info.seq - (int64_t) stream->expected;
        delta1 = delta + 0x100000000L;
        delta2 = delta - 0x100000000L;

        if (llabs(delta2) < llabs(delta1))
            delta1 = delta2;

        if (llabs(delta1) < llabs(delta))
            delta = delta1;

        if (delta < 0) {
            // received lost packet
            fprintf(stderr, "[%s] expected %lu, got %lu: dropped\n",
                    info.name, (long unsigned) stream->expected,
                    (long unsigned) info.seq);
            continue;
        }

        // lost packets
        if (delta == 1)
            fprintf(stderr, "[%s] expected %lu, got %lu: lost 1 packet\n",
                    info.name, (long unsigned) stream->expected,
                    (long unsigned) info.seq);
        else
            fprintf(stderr, "[%s] expected %lu, got %lu: lost %lld packets\n",
                    info.name, (long unsigned) stream->expected,
                    (long unsigned) info.seq, (long long) delta);

        if (stream->prev)
            free(stream->prev);

        if (stream->curr)
            free(stream->curr);

        stream->expected = info.seq + 1;
        stream->tsprev = (struct timespec){ 0, 0 };
        stream->tscurr = tspec;
        stream->prev = NULL;
        stream->curr = buffer;
        return stream;
    }
}

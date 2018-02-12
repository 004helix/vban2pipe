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
#include <net/if.h>
#include <errno.h>

#include "vban.h"
#include "logger.h"
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

        logger(LOG_INF, "[%s@%s] stream offline", del->name, del->ifname);

        if (del->curr.data)
            free(del->curr.data);

        if (del->prev.data)
            free(del->prev.data);

        free(del);
    }

    streams = NULL;
}


/*
 * Remove stream
 */
void forgetstream(struct stream *stream)
{
    logger(LOG_INF, "[%s@%s] stream offline", stream->name, stream->ifname);

    if (stream == streams) {
        streams = stream->next;
    } else {
        struct stream *prev;
        for (prev = streams; prev->next != stream; prev = prev->next);
        prev->next = stream->next;
    }

    if (stream->curr.data)
        free(stream->curr.data);

    if (stream->prev.data)
        free(stream->prev.data);

    free(stream);
}


/*
 * Find stream for the packet
 */
struct stream *getstream(struct vbaninfo *info, struct sockaddr *addr, unsigned ifindex)
{
    struct stream *stream;

    if (!streams)
        return NULL;

    for (stream = streams; stream; stream = stream->next) {
        struct sockaddr *peer = (void *) &stream->peer;

        // check interface
        if (stream->ifindex != ifindex)
            continue;

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
    unsigned ifindex;
    struct stream *stream;
    struct sockaddr_storage addr;
    struct vbaninfo info;
    struct iovec iov[2];
    struct timespec ts;
    struct cmsghdr *cm;
    struct msghdr m;
    int found_idx;
    int found_ts;
    ssize_t size;

    buffer = malloc(DATA_BUFFER_SIZE);
    if (!buffer) {
        logger(LOG_ERR, "cannot allocate memory!");
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

        if (size < 0 && errno == EAGAIN) {
            free(buffer);
            return NULL;
        }

        if (size < 0) {
            logger(LOG_ERR, "recvmsg: %s", strerror(errno));
            free(buffer);
            return NULL;
        }

        ifindex = 0;
        found_ts = 0;
        found_idx = 0;
        for (cm = CMSG_FIRSTHDR(&m); cm; cm = CMSG_NXTHDR(&m, cm)) {
            if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_PKTINFO) {
                struct in_pktinfo *ipi = (void *) CMSG_DATA(cm);
                ifindex = ipi->ipi_ifindex;
                found_idx++;
            }
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_TIMESTAMPNS) {
                memcpy(&ts, CMSG_DATA(cm), sizeof(ts));
                found_ts++;
            }
        }

        if (!found_idx) {
            logger(LOG_ERR, "couldn't find IP_PKTINFO data in auxiliary recvmsg() data!");
            free(buffer);
            return NULL;
        }

        if (!found_ts) {
            logger(LOG_ERR, "couldn't find SCM_TIMESTAMPNS data in auxiliary recvmsg() data!");
            free(buffer);
            return NULL;
        }

        if (vban_parse(vban_header, size, &info) < 0) {
            logger(LOG_VRB, "malformed VBAN packet received");
            continue;
        }

        if (info.protocol != VBAN_PROTOCOL_AUDIO) {
            logger(LOG_VRB, "[%s] unsupporded protocol", info.name);
            continue;
        }

        if (info.codec != VBAN_CODEC_PCM) {
            logger(LOG_VRB, "[%s] unsupported audio codec", info.name);
            continue;
        }

        stream = getstream(&info, (struct sockaddr *) &addr, ifindex);
        size -= VBAN_HEADER_SIZE;

        if (stream) {
            double dt, dv;

            // check packet size
            if (size < stream->datasize) {
                logger(LOG_VRB, "[%s@%s] too short packet received",
                       stream->name, stream->ifname);
                continue;
            } else
            if (size > stream->datasize) {
                logger(LOG_VRB, "[%s@%s] too long packet received",
                       stream->name, stream->ifname);
                continue;
            }

            // check packet type
            if (stream->samples != info.samples ||
                stream->datatype != info.datatype ||
                stream->channels != info.channels ||
                stream->sample_rate != info.sample_rate) {
                logger(LOG_VRB, "[%s@%s] bad packet received",
                       stream->name, stream->ifname);
                continue;
            }

            // update stats and timestamp
            dt = (ts.tv_sec - stream->ts_last.tv_sec) * 1000000000.0 +
                 (ts.tv_nsec - stream->ts_last.tv_nsec);
            // exponentially weighted moving variance
            dv = dt - stream->dt_average;
            stream->dt_variance = stream->ewma_a2 *
                                  (stream->dt_variance + stream->ewma_a1 * dv * dv);
            // exponentially weighted moving average
            stream->dt_average = stream->ewma_a1 * dt +
                                 stream->ewma_a2 * stream->dt_average;
            // stream timestamp
            stream->ts_last = ts;

        } else {
            char peer[128];
            double pps;

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
                    logger(LOG_ERR, "[%s] unsupported address family", info.name);
                    continue;
            }

            // create new stream entry
            stream = malloc(sizeof(struct stream));
            if (!stream) {
                logger(LOG_ERR, "[%s] cannot allocate memory", info.name);
                free(buffer);
                return NULL;
            }

            // parse interface index
            stream->ifindex = ifindex;
            if_indextoname(ifindex, stream->ifname);

            memcpy(&stream->peer, &addr, sizeof(struct sockaddr_storage));
            strcpy(stream->name, info.name);

            stream->samples = info.samples;
            stream->sample_size = info.sample_size;
            stream->sample_rate = info.sample_rate;
            stream->channels = info.channels;
            stream->datasize = size;
            stream->datatype = info.datatype;
            stream->format = info.format;

            stream->lost = 0;
            stream->expected = info.seq;
            stream->curr.data = NULL;
            stream->prev.data = NULL;
            stream->ts_first = ts;
            stream->ts_last = ts;

            stream->ignore = 0;
            stream->insync = 0;
            stream->offset = 0;

            stream->next = NULL;

            // stats
            pps = (double) stream->sample_rate / (double) stream->samples;
            stream->ewma_a1 = 2.0 / (1.0 + 30.0 * pps);
            stream->ewma_a2 = 1.0 - stream->ewma_a1;
            stream->dt_average = 1000000000.0 / pps;
            stream->dt_variance = 0;

            logger(LOG_INF, "[%s@%s] stream connected from %s, %s, %ld Hz, %ld channel(s)",
                   stream->name, stream->ifname, peer, stream->format,
                   stream->sample_rate, stream->channels);

            if (streams) {
                struct stream *tail = streams;
                for (; tail->next; tail = tail->next);
                tail->next = stream;
            } else {
                streams = stream;
            }
        }

        // save data to stream buffers
        if (stream->expected == info.seq) {
            if (stream->prev.data)
                free(stream->prev.data);

            stream->expected++;
            stream->prev = stream->curr;
            stream->curr.data = buffer;
            stream->curr.sent = 0;
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
            if (delta == -1 || (delta == -2 && stream->prev.data)) {
                // duplicate
                logger(LOG_DBG, "[%s@%s] expected %lu, got %lu: duplicate?",
                       stream->name, stream->ifname, (long unsigned) stream->expected,
                       (long unsigned) info.seq);
                continue;
            }

            // received previous packet
            if (delta == -2 && !stream->prev.data) {
                // restore previous packet
                stream->lost--;
                stream->prev.data = buffer;
                stream->prev.sent = 0;

                logger(LOG_DBG, "[%s@%s] expected %lu, got %lu: restored",
                       stream->name, stream->ifname, (long unsigned) stream->expected,
                       (long unsigned) info.seq);

                return stream;
            }

            logger(LOG_DBG, "[%s@%s] expected %lu, got %lu: dropped",
                   stream->name, stream->ifname, (long unsigned) stream->expected,
                   (long unsigned) info.seq);
            continue;
        }

        // lost packets
        stream->lost += (long) delta;
        if (delta == 1)
            logger(LOG_DBG, "[%s@%s] expected %lu, got %lu: lost 1 packet",
                   stream->name, stream->ifname, (long unsigned) stream->expected,
                   (long unsigned) info.seq);
        else
            logger(LOG_DBG, "[%s@%s] expected %lu, got %lu: lost %lld packets",
                   stream->name, stream->ifname, (long unsigned) stream->expected,
                   (long unsigned) info.seq, (long long) delta);

        if (stream->prev.data)
            free(stream->prev.data);

        if (stream->curr.data)
            free(stream->curr.data);

        stream->expected = info.seq + 1;
        stream->prev.data = NULL;
        stream->prev.sent = 0;
        stream->curr.data = buffer;
        stream->curr.sent = 0;
        return stream;
    }
}

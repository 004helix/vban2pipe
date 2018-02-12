/*
 *  VBAN Receiver
 *
 *  Copyright (C) 2017, 2018 Raman Shyshniou <rommer@ibuffed.com>
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
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#include <alloca.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "vban.h"
#include "streams.h"
#include "output.h"
#include "logger.h"
#include "httpd.h"


#define STREAM_TIMEOUT_MSEC 700
#define BUFFER_OUT_PACKETS 2


/*
 * global vars
 */

static char *pipename = NULL;
static char *onconnect = NULL;
static char *ondisconnect = NULL;


static void error(char *msg, int err)
{
    logger(LOG_ERR, "%s: %s", msg, strerror(err));
    exit(1);
}


static void runhook(char *prog)
{
    char *argv[2];
    pid_t pid = fork();

    switch (pid) {
        case -1:
            logger(LOG_ERR, "exec failed, unable to fork: %s", strerror(errno));
            return;
        case 0:
            argv[0] = prog;
            argv[1] = NULL;
            execvp(prog, argv);
            logger(LOG_ERR, "execvp failed: %s", strerror(errno));
            exit(1);
        break;
    }
}


static int syncstreams(struct stream *stream1, struct stream *stream2, int64_t *offset)
{
    int i, w, matches;
    long size = stream2->datasize;
    char *data1, data2[size * 2];

    // compare streams
    if (stream1->samples != stream2->samples ||
        stream1->datatype != stream2->datatype ||
        stream1->channels != stream2->channels ||
        stream1->sample_rate != stream2->sample_rate)
        return -1;

    // try to find last packet of stream1
    // in the last two packets of stream2

    if (!stream2->prev.data)
        // not enough consequitive packets in stream2
        return 0;

    data1 = stream1->curr.data;
    memcpy(data2, stream2->prev.data, size);
    memcpy(data2 + size, stream2->curr.data, size);

    matches = 0;
    w = stream1->sample_size * stream1->channels;
    for (i = 0; i <= size; i += w)
        if (!memcmp(data1, data2 + i, size)) {
            if (matches == 0) {
                *offset = (int64_t) stream2->expected - 1;
                *offset -= (int64_t) stream1->expected;
                *offset *= stream1->samples;
                *offset += i / w;
            }
            matches++;
        }

    return matches;
}


static void run(int sock)
{
    struct stream *stream, *dead;
    time_t stat_sec = 0;

    for (;;) {
        stream = recvvban(sock);

        if (!stream)
            return;

        // snapshot streams every second
        if (stream->ts_last.tv_sec != stat_sec) {
            stat_sec = stream->ts_last.tv_sec;
            httpd_update(streams);
        }

        // check dead streams
        for (dead = streams; dead; dead = dead->next) {
            long msec;

            // skip current stream
            if (dead == stream)
                continue;

            msec = (long) (stream->ts_last.tv_sec - dead->ts_last.tv_sec) * 1000L;
            msec += (long) (stream->ts_last.tv_nsec - dead->ts_last.tv_nsec) / 1000000L;

            if (msec < STREAM_TIMEOUT_MSEC)
                continue;

            if (dead == streams) {
                // primary stream died
                // fix offsets and output position
                struct stream *curr;
                int64_t delta;

                if (!streams->next)
                    // last stream timed out
                    return;

                delta = dead->next->offset;

                for (curr = dead->next; curr; curr = curr->next)
                    curr->offset -= delta;

                output_move(delta);
            }

            forgetstream(dead);
        }

        // ignore stream
        if (stream->ignore)
            continue;

        // stream sync paused
        if (stream->insync < 0) {
            stream->insync++;
            continue;
        }

        // require 3 successfully attempts to sync
        if (stream->insync < 3) {
            int64_t offset;
            int matches;

            if (stream == streams) {
                logger(LOG_INF, "[%s@%s] stream online, primary",
                       stream->name, stream->ifname);

                if (output_init(pipename, stream) < 0)
                    error("pipe open", errno);

                if (onconnect)
                    runhook(onconnect);

                stream->insync = 3;
                continue;
            }

            matches = syncstreams(streams, stream, &offset);

            if (matches < 0) {
                logger(LOG_INF, "[%s@%s] stream didnt match primary stream, ignoring",
                       stream->name, stream->ifname);
                stream->ignore++;
                continue;
            }

            if (matches == 0) {
                matches = syncstreams(stream, streams, &offset);
                offset = -offset;
            }

            if (matches == 1) {
                if (stream->insync++ && stream->offset != offset) {
                    // offset mismatch, pause (~100ms) and try to sync again
                    stream->insync = -(long) (stream->sample_rate / stream->samples / 10);
                    continue;
                }

                if (stream->insync == 3)
                    logger(LOG_INF, "[%s@%s] stream online, offset %lld samples",
                           stream->name, stream->ifname, (long long) offset);

                stream->offset = offset;
            } else {
                if (stream->insync == 0)
                    // still cant sync, pause (~100ms) stream for a while
                    stream->insync = -(long) (stream->sample_rate / stream->samples / 10);
            }

            continue;
        }

        if (stream->prev.data && !stream->prev.sent) {
            output_play(stream->samples * (stream->expected - 1) - stream->offset,
                        stream->samples, stream->prev.data, stream->datasize);
            stream->prev.sent++;
        }

        if (!stream->curr.sent) {
            output_play(stream->samples * stream->expected - stream->offset,
                        stream->samples, stream->curr.data, stream->datasize);
            stream->curr.sent++;
        }
    }
}


int main(int argc, char **argv)
{
    struct sockaddr_in addr;
    struct timeval timeout;
    int vbsock, httpdsock;
    int port, optval;

    logger_init();

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    // check command line arguments
    if (argc < 3) {
        logger(LOG_ERR, "usage: %s <port> <pipe> [exec-on-connect] [exec-on-disconnect]", argv[0]);
        return 1;
    }

    // parse port
    port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        logger(LOG_ERR, "bad port: %s", argv[1]);
        return 1;
    }

    // setup pipename, connect/disconnect handlers
    if (!(pipename = strdup(argv[2])))
        error("strdup", ENOMEM);

    if (argc > 3 && !(onconnect = strdup(argv[3])))
        error("strdup", ENOMEM);

    if (argc > 4 && !(ondisconnect = strdup(argv[4])))
        error("strdup", ENOMEM);

    // create UDP (vban) listen socket
    vbsock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (vbsock < 0)
        error("socket", errno);

    // set SO_REUSEADDR
    optval = 1;
    if (setsockopt(vbsock, SOL_SOCKET, SO_REUSEADDR,
                   (const void *)&optval, sizeof(optval)) < 0)
        error("setsockopt failed", errno);

    // address to listen on
    bzero((char *) &addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    // bind
    if (bind(vbsock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        error("bind", errno);

    // set receive timeout
    timeout.tv_sec = STREAM_TIMEOUT_MSEC / 1000000;
    timeout.tv_usec = (STREAM_TIMEOUT_MSEC % 100000) * 1000;
    if (setsockopt(vbsock, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) < 0)
        error("setsockopt failed", errno);

    // set SO_TIMESTAMPNS
    optval = 1;
    if (setsockopt(vbsock, SOL_SOCKET, SO_TIMESTAMPNS, &optval,
                   sizeof(optval)) < 0)
        error("setsockopt failed", errno);

    // set IP_PKTINFO
    optval = 1;
    if (setsockopt(vbsock, IPPROTO_IP, IP_PKTINFO, &optval,
                   sizeof(optval)) < 0)
        error("setsockopt failed", errno);

    // create TCP (httpd) listen socket
    httpdsock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (httpdsock < 0)
        error("socket", errno);

    // set SO_REUSEADDR
    optval = 1;
    if (setsockopt(httpdsock, SOL_SOCKET, SO_REUSEADDR,
                   (const void *)&optval, sizeof(optval)) < 0)
        error("setsockopt failed", errno);

    // address to listen on
    bzero((char *) &addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    // bind
    if (bind(httpdsock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        error("bind", errno);

    // listen
    if (listen(httpdsock, 63) < 0)
        error("listen", errno);

    // start httpd server
    if (httpd(httpdsock) < 0)
        error("httpd start", errno);

    // vban receive loop
    while (1) {
        run(vbsock);

        // disconnect all streams
        if (streams) {
            forgetstreams();

            if (output_done() < 0)
                error("pipe close", errno);

            if (ondisconnect)
                runhook(ondisconnect);

            // update streams stats
            httpd_update(NULL);
        }
    }

    return 0;
}

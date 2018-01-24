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

#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#include "httpd.h"
#include "logger.h"
#include "streams.h"


static struct stream_stat_cell *stats = NULL;
static struct stream_stat_cell cell1 = { NULL, 0 };
static struct stream_stat_cell cell2 = { NULL, 0 };
static struct stream_stat_cell cell3 = { NULL, 0 };


static char err405[] = "HTTP/1.0 405 Method Not Allowed\r\n"
                       "Server: vban2pipe\r\n"
                       "Content-Length: 0\r\n"
                       "Connection: close\r\n"
                       "\r\n";

static char ok200[] = "HTTP/1.0 200 OK\r\n"
                      "Server: vban2pipe\r\n"
                      "Content-Length: %d\r\n"
                      "Connection: close\r\n"
                      "\r\n";


static int json_escape(char *dst, char *src)
{
    int n;

    for (n = 0; *src; n++, src++)
        switch (*src) {
            case '"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                *dst++ = '\\';
                *dst++ = *src;
                n++;
                break;
            default:
                if (0 >= *src && *src <= 0x1f) {
                    dst += sprintf(dst, "\\u00%02x", *src);
                    n += 5;
                } else
                    *dst++ = *src;
        }

    return n;
}


static char *json_dump(int count, struct stream_stat *ss)
{
    static size_t buffer_size = 0;
    static char *buffer = NULL;
    int len, i;

    if (!buffer) {
        buffer = malloc(4096);
        if (!buffer)
            return NULL;

        buffer_size = 4096;
    }

    len = sprintf(buffer, "{\"count\":%d,\"list\":[", count);

    if (count > 0) {
        strcat(buffer, "\n");
        len++;
    }

    for (i = 0; i < count; i++) {
        if (buffer_size - len < 1024) {
            char *newbuffer = realloc(buffer, buffer_size + 4096);

            if (!newbuffer)
                return NULL;

            buffer_size += 4096;
            buffer = newbuffer;
        }

        strcat(buffer + len, "  {\"name\":\"");
        len += 10;

        len += json_escape(buffer + len, ss[i].name);

        strcat(buffer + len, "\",\"ifname\":\"");
        len += 12;

        len += json_escape(buffer + len, ss[i].ifname);

        strcat(buffer + len, "\"}\n");
        len += 3;
    }

    strcat(buffer + len, "]}");

    return buffer;
}


static void *httpd_accept(void *userdata)
{
    int sock, lsock = *((int *) userdata);
    struct stream_stat_cell *cell;
    struct sockaddr_storage addr;
    struct timeval timeout;
    struct timespec ts;
    char buffer[8192];
    socklen_t len;
    char *json;
    int size;

    while (1) {
        len = sizeof(addr);
        sock = accept(lsock, (struct sockaddr *)&addr, &len);

        if (sock == -1)
            switch (errno) {
                // not enough resources
                case EMFILE:
                case ENOBUFS:
                case ENOMEM:
                case EPERM:
                    // sleep (100ms) and retry
                    ts.tv_sec = 0;
                    ts.tv_nsec = 100000000;
                    nanosleep(&ts, NULL);

                // not a fatal error
                case ECONNABORTED:
                case EINTR:
                    continue;

                // unknown or fatal error
                default:
                    logger(LOG_ERR, "accept error: %s", strerror(errno));
                    logger(LOG_ERR, "httpd thread stopped");
                    close(lsock);
                    return NULL;
            }

        // set receive timeout
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
            logger(LOG_INF, "setsockopt(SO_RCVTIMEO) failed: %s", strerror(errno));
            close(sock);
            continue;
        }

        // set send timeout
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
            logger(LOG_INF, "setsockopt(SO_SNDTIMEO) failed: %s", strerror(errno));
            close(sock);
            continue;
        }

        // read http request
        for (size = 0; size < sizeof(buffer);) {
            ssize_t rc;

            rc = read(sock, buffer + size, sizeof(buffer) - size);

            if (rc == -1 && errno == EINTR)
                continue;

            if (rc <= 0) {
                size = -1;
                break;
            }

            size += rc;

            if (size >= 2 &&
                buffer[size - 1] == '\n' && buffer[size - 2] == '\n')
                break;

            if (size >= 4 &&
                buffer[size - 1] == '\n' && buffer[size - 2] == '\r' &&
                buffer[size - 3] == '\n' && buffer[size - 4] == '\r')
                break;
        }

        // read error or too short request
        if (size < 4) {
            close(sock);
            continue;
        }

        // bad or too long request
        if ((buffer[size - 1] != '\n' || buffer[size - 2] != '\n') &&
            (buffer[size - 1] != '\n' || buffer[size - 2] != '\r' ||
             buffer[size - 3] != '\n' || buffer[size - 4] != '\r')) {
            close(sock);
            continue;
        }

        // only GET requests are supported
        if (size < 4 || strncasecmp("GET ", buffer, 4)) {
            write(sock, err405, strlen(err405));
            close(sock);
            continue;
        }

        // atomic operation
        cell = stats;

        // dump statistic
        if (cell)
            json = json_dump(cell->count, cell->ss);
        else
            json = json_dump(0, NULL);
        size = sprintf(buffer, ok200, strlen(json));
        write(sock, buffer, size);
        write(sock, json, strlen(json));
        close(sock);
    }

    return NULL;
}


void httpd_update(struct stream *streams)
{
    struct stream_stat_cell *cell;
    struct stream *stream;
    int i, count;

    // no streams connected, no stats
    if (!streams) {
        stats = NULL;
        return;
    }

    // select cell to save
    if (stats == &cell1)
        cell = &cell2;
    else
    if (stats == &cell2)
        cell = &cell3;
    else
        cell = &cell1;

    // total streams
    for (count = 0, stream = streams; stream; stream = stream->next)
        count++;

    // check available memory size
    if (cell->count < count)
        cell->ss = realloc(cell->ss, count * sizeof(struct stream_stat));

    if (cell->ss == NULL) {
        cell->count = 0;
        return;
    }

    // save streams stat
    for (i = 0, stream = streams; stream; i++, stream = stream->next) {
        strcpy(cell->ss[i].ifname, stream->ifname);
        strcpy(cell->ss[i].name, stream->name);

        cell->ss[i].peer        = stream->peer;
        cell->ss[i].dtname      = stream->dtname;
        cell->ss[i].sample_rate = stream->sample_rate;
        cell->ss[i].channels    = stream->channels;
        cell->ss[i].expected    = stream->expected;
        cell->ss[i].lost        = stream->lost;
        cell->ss[i].ignore      = stream->ignore;
        cell->ss[i].insync      = stream->insync;
        cell->ss[i].offset      = stream->offset;
    }

    // update stats pointer
    // this is atomic operation, no need to lock
    stats = cell;
}


int httpd(sock)
{
    pthread_attr_t attr;
    pthread_t thread;
    static int arg;
    int rc;

    if ((rc = pthread_attr_init(&attr))) {
        logger(LOG_ERR, "pthread_attr_init failed: %s", strerror(-rc));
        return -1;
    }

    if ((rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED))) {
        logger(LOG_ERR, "pthread_attr_setdetachstate failed: %s", strerror(-rc));
        return -1;
    }

    arg = sock;
    if ((rc = pthread_create(&thread, &attr, httpd_accept, &arg))) {
        logger(LOG_ERR, "pthread_create failed: %s", strerror(-rc));
        return -1;
    }

    return 0;
}
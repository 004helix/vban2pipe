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
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "logger.h"

static char buffer[16384];
static int verbose;


void logger_init(void)
{
    verbose = LOG_INF;

    if (getenv("VERBOSE"))
        verbose = LOG_VRB;

    if (getenv("DEBUG"))
        verbose = LOG_DBG;
}


void logger(int level, const char* format, ...)
{
    if (level <= verbose) {
        va_list ap;
        int size;

        va_start(ap, format);
        size = vsnprintf(buffer, sizeof(buffer), format, ap);
        va_end(ap);

        if (!size || buffer[size - 1] != '\n')
            buffer[size++] = '\n';

        write(STDERR_FILENO, buffer, (size_t) size);
    }
}

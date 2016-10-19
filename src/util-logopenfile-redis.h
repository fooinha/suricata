/* Copyright (C) 2007-2016 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Paulo Pacheco <fooinha@gmail.com>
 */

#ifndef __UTIL_LOGOPENFILE_REDIS_H__
#define __UTIL_LOGOPENFILE_REDIS_H__

#ifdef HAVE_LIBHIREDIS
#include <hiredis/hiredis.h>

#ifdef HAVE_LIBEVENT
#include <hiredis/async.h>
#endif /* HAVE_LIBEVENT */

#include "conf.h"            /* ConfNode   */

enum RedisMode { REDIS_LIST, REDIS_CHANNEL };

typedef struct RedisSetup_ {
    enum RedisMode mode;
    const char *command;
    char *key;
    int  batch_size;
    int  batch_count;
    char *server;
    int  port;
    time_t tried;
    int async;
} RedisSetup;

typedef struct SCLogRedisContext_ {
       redisContext *sync;

#if HAVE_LIBEVENT
       redisAsyncContext *async;
       struct event_base *ev_base;
#endif /* HAVE_LIBEVENT */

} SCLogRedisContext;

int SCConfLogOpenRedis(ConfNode *, void *);
int LogFileWriteRedis(void *, const char *, size_t);
void SCLogRedisContextFree(SCLogRedisContext *, int);

#endif /* HAVE_LIBHIREDIS */
#endif /* __UTIL_LOGOPENFILE_REDIS_H__ */
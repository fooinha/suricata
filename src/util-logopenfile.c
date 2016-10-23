/* vi: set et ts=4: */
/* Copyright (C) 2007-2014 Open Information Security Foundation
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
 * \author Mike Pomraning <mpomraning@qualys.com>
 *
 * Kafka output
 * \author Paulo Pacheco <fooinha@gmail.com>
 *
 * File-like output for logging:  regular files and sockets.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "suricata-common.h" /* errno.h, string.h, etc. */
#include "tm-modules.h"      /* LogFileCtx */
#include "conf.h"            /* ConfNode, etc. */
#include "output.h"          /* DEFAULT_LOG_* */
#include "util-logopenfile.h"
#include "util-logopenfile-tile.h"
#include "util-print.h"

#ifdef HAVE_LIBHIREDIS
#include <hiredis/hiredis.h>

const char * redis_push_cmd = "LPUSH";
const char * redis_publish_cmd = "PUBLISH";


#if HAVE_LIBEVENT == 1

#include <hiredis/async.h>
#include <hiredis/adapters/libevent.h>

#endif // HAVE_LIBEVENT

typedef struct SCLogRedisContext_ {
	redisContext *sync;

#if HAVE_LIBEVENT == 1
	redisAsyncContext *async;
	struct event_base *ev_base;
#endif // HAVE_LIBEVENT

} SCLogRedisContext;

#endif // HAVE_LIBHIREDIS

/** \brief connect to the indicated local stream socket, logging any errors
 *  \param path filesystem path to connect to
 *  \param log_err, non-zero if connect failure should be logged.
 *  \retval FILE* on success (fdopen'd wrapper of underlying socket)
 *  \retval NULL on error
 */
static FILE *
SCLogOpenUnixSocketFp(const char *path, int sock_type, int log_err)
{
    struct sockaddr_un saun;
    int s = -1;
    FILE * ret = NULL;

    memset(&saun, 0x00, sizeof(saun));

    s = socket(PF_UNIX, sock_type, 0);
    if (s < 0) goto err;

    saun.sun_family = AF_UNIX;
    strlcpy(saun.sun_path, path, sizeof(saun.sun_path));

    if (connect(s, (const struct sockaddr *)&saun, sizeof(saun)) < 0)
        goto err;

    ret = fdopen(s, "w");
    if (ret == NULL)
        goto err;

    return ret;

err:
    if (log_err)
        SCLogWarning(SC_ERR_SOCKET,
            "Error connecting to socket \"%s\": %s (will keep trying)",
            path, strerror(errno));

    if (s >= 0)
        close(s);

    return NULL;
}

/**
 * \brief Attempt to reconnect a disconnected (or never-connected) Unix domain socket.
 * \retval 1 if it is now connected; otherwise 0
 */
static int SCLogUnixSocketReconnect(LogFileCtx *log_ctx)
{
    int disconnected = 0;
    if (log_ctx->fp) {
        SCLogWarning(SC_ERR_SOCKET,
            "Write error on Unix socket \"%s\": %s; reconnecting...",
            log_ctx->filename, strerror(errno));
        fclose(log_ctx->fp);
        log_ctx->fp = NULL;
        log_ctx->reconn_timer = 0;
        disconnected = 1;
    }

    struct timeval tv;
    uint64_t now;
    gettimeofday(&tv, NULL);
    now = (uint64_t)tv.tv_sec * 1000;
    now += tv.tv_usec / 1000;           /* msec resolution */
    if (log_ctx->reconn_timer != 0 &&
            (now - log_ctx->reconn_timer) < LOGFILE_RECONN_MIN_TIME) {
        /* Don't bother to try reconnecting too often. */
        return 0;
    }
    log_ctx->reconn_timer = now;

    log_ctx->fp = SCLogOpenUnixSocketFp(log_ctx->filename, log_ctx->sock_type, 0);
    if (log_ctx->fp) {
        /* Connected at last (or reconnected) */
        SCLogNotice("Reconnected socket \"%s\"", log_ctx->filename);
    } else if (disconnected) {
        SCLogWarning(SC_ERR_SOCKET, "Reconnect failed: %s (will keep trying)",
            strerror(errno));
    }

    return log_ctx->fp ? 1 : 0;
}

/**
 * \brief Write buffer to log file.
 * \retval 0 on failure; otherwise, the return value of fwrite (number of
 * characters successfully written).
 */
static int SCLogFileWrite(const char *buffer, int buffer_len, LogFileCtx *log_ctx)
{
    /* Check for rotation. */
    if (log_ctx->rotation_flag) {
        log_ctx->rotation_flag = 0;
        SCConfLogReopen(log_ctx);
    }

    int ret = 0;

    if (log_ctx->fp == NULL && log_ctx->is_sock)
        SCLogUnixSocketReconnect(log_ctx);

    if (log_ctx->fp) {
        clearerr(log_ctx->fp);
        ret = fwrite(buffer, buffer_len, 1, log_ctx->fp);
        fflush(log_ctx->fp);

        if (ferror(log_ctx->fp) && log_ctx->is_sock) {
            /* Error on Unix socket, maybe needs reconnect */
            if (SCLogUnixSocketReconnect(log_ctx)) {
                ret = fwrite(buffer, buffer_len, 1, log_ctx->fp);
                fflush(log_ctx->fp);
            }
        }
    }

    return ret;
}

static void SCLogFileClose(LogFileCtx *log_ctx)
{
    if (log_ctx->fp)
        fclose(log_ctx->fp);
}

/** \brief open the indicated file, logging any errors
 *  \param path filesystem path to open
 *  \param append_setting open file with O_APPEND: "yes" or "no"
 *  \retval FILE* on success
 *  \retval NULL on error
 */
static FILE *
SCLogOpenFileFp(const char *path, const char *append_setting)
{
    FILE *ret = NULL;

    if (ConfValIsTrue(append_setting)) {
        ret = fopen(path, "a");
    } else {
        ret = fopen(path, "w");
    }

    if (ret == NULL)
        SCLogError(SC_ERR_FOPEN, "Error opening file: \"%s\": %s",
                   path, strerror(errno));
    return ret;
}

/** \brief open the indicated file remotely over PCIe to a host
 *  \param path filesystem path to open
 *  \param append_setting open file with O_APPEND: "yes" or "no"
 *  \retval FILE* on success
 *  \retval NULL on error
 */
static PcieFile *SCLogOpenPcieFp(LogFileCtx *log_ctx, const char *path, 
                                 const char *append_setting)
{
#ifndef __tile__
    SCLogError(SC_ERR_INVALID_YAML_CONF_ENTRY,
               "PCIe logging only supported on Tile-Gx Architecture.");
    return NULL;
#else
    return TileOpenPcieFp(log_ctx, path, append_setting);
#endif
}

/** \brief open a generic output "log file", which may be a regular file or a socket
 *  \param conf ConfNode structure for the output section in question
 *  \param log_ctx Log file context allocated by caller
 *  \param default_filename Default name of file to open, if not specified in ConfNode
 *  \param rotate Register the file for rotation in HUP.
 *  \retval 0 on success
 *  \retval -1 on error
 */
int
SCConfLogOpenGeneric(ConfNode *conf,
                     LogFileCtx *log_ctx,
                     const char *default_filename,
                     int rotate)
{
    char log_path[PATH_MAX];
    char *log_dir;
    const char *filename, *filetype;

    // Arg check
    if (conf == NULL || log_ctx == NULL || default_filename == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENT,
                   "SCConfLogOpenGeneric(conf %p, ctx %p, default %p) "
                   "missing an argument",
                   conf, log_ctx, default_filename);
        return -1;
    }
    if (log_ctx->fp != NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENT,
                   "SCConfLogOpenGeneric: previously initialized Log CTX "
                   "encountered");
        return -1;
    }

    // Resolve the given config
    filename = ConfNodeLookupChildValue(conf, "filename");
    if (filename == NULL)
        filename = default_filename;

    log_dir = ConfigGetLogDirectory();

    if (PathIsAbsolute(filename)) {
        snprintf(log_path, PATH_MAX, "%s", filename);
    } else {
        snprintf(log_path, PATH_MAX, "%s/%s", log_dir, filename);
    }

    filetype = ConfNodeLookupChildValue(conf, "filetype");
    if (filetype == NULL)
        filetype = DEFAULT_LOG_FILETYPE;

    const char *append = ConfNodeLookupChildValue(conf, "append");
    if (append == NULL)
        append = DEFAULT_LOG_MODE_APPEND;

    // Now, what have we been asked to open?
    if (strcasecmp(filetype, "unix_stream") == 0) {
        /* Don't bail. May be able to connect later. */
        log_ctx->is_sock = 1;
        log_ctx->sock_type = SOCK_STREAM;
        log_ctx->fp = SCLogOpenUnixSocketFp(log_path, SOCK_STREAM, 1);
    } else if (strcasecmp(filetype, "unix_dgram") == 0) {
        /* Don't bail. May be able to connect later. */
        log_ctx->is_sock = 1;
        log_ctx->sock_type = SOCK_DGRAM;
        log_ctx->fp = SCLogOpenUnixSocketFp(log_path, SOCK_DGRAM, 1);
    } else if (strcasecmp(filetype, DEFAULT_LOG_FILETYPE) == 0 ||
               strcasecmp(filetype, "file") == 0) {
        log_ctx->fp = SCLogOpenFileFp(log_path, append);
        if (log_ctx->fp == NULL)
            return -1; // Error already logged by Open...Fp routine
        log_ctx->is_regular = 1;
        if (rotate) {
            OutputRegisterFileRotationFlag(&log_ctx->rotation_flag);
        }
    } else if (strcasecmp(filetype, "pcie") == 0) {
        log_ctx->pcie_fp = SCLogOpenPcieFp(log_ctx, log_path, append);
        if (log_ctx->pcie_fp == NULL)
            return -1; // Error already logged by Open...Fp routine
    } else {
        SCLogError(SC_ERR_INVALID_YAML_CONF_ENTRY, "Invalid entry for "
                   "%s.filetype.  Expected \"regular\" (default), \"unix_stream\", "
                   "\"pcie\" "
                   "or \"unix_dgram\"",
                   conf->name);
    }
    log_ctx->filename = SCStrdup(log_path);
    if (unlikely(log_ctx->filename == NULL)) {
        SCLogError(SC_ERR_MEM_ALLOC,
            "Failed to allocate memory for filename");
        return -1;
    }

    SCLogInfo("%s output device (%s) initialized: %s", conf->name, filetype,
              filename);

    return 0;
}

/**
 * \brief Reopen a regular log file with the side-affect of truncating it.
 *
 * This is useful to clear the log file and start a new one, or to
 * re-open the file after its been moved by something external
 * (eg. logrotate).
 */
int SCConfLogReopen(LogFileCtx *log_ctx)
{
    if (!log_ctx->is_regular) {
        /* Not supported and not needed on non-regular files. */
        return 0;
    }

    if (log_ctx->filename == NULL) {
        SCLogWarning(SC_ERR_INVALID_ARGUMENT,
            "Can't re-open LogFileCtx without a filename.");
        return -1;
    }

    fclose(log_ctx->fp);

    /* Reopen the file. Append is forced in case the file was not
     * moved as part of a rotation process. */
    SCLogDebug("Reopening log file %s.", log_ctx->filename);
    log_ctx->fp = SCLogOpenFileFp(log_ctx->filename, "yes");
    if (log_ctx->fp == NULL) {
        return -1; // Already logged by Open..Fp routine.
    }

    return 0;
}


#ifdef HAVE_LIBHIREDIS

/** \brief SCLogRedisContextAlloc() - Allocates and initalizes redis context
 *  \param async indicates that async mode will be used
 *  \retval SCLogRedisContext * pointer if succesful, EXIT_FAILURE program if not
 */
static SCLogRedisContext * SCLogRedisContextAlloc(int async)
{

    SCLogRedisContext* ctx = (SCLogRedisContext*) SCMalloc(sizeof(SCLogRedisContext));

	if (unlikely(ctx == NULL)) {
        SCLogError(SC_ERR_MEM_ALLOC, "Unable to allocate redis context!");
        exit(EXIT_FAILURE);
	}

	ctx->sync    = NULL;

#if HAVE_LIBEVENT == 1
	ctx->ev_base = NULL;
	ctx->async   = NULL;

    if (async) {
        ctx->ev_base = event_base_new();
        if (unlikely(ctx->ev_base == NULL)) {
            SCLogError(SC_ERR_MEM_ALLOC, "Unable to allocate redis async event base!");
            exit(EXIT_FAILURE);
        }
    }
#endif

	return ctx;
}

/** \brief SCLogRedisContextFree() free redis context
 *  \param async indicates that async mode was used
 */
static void SCLogRedisContextFree(SCLogRedisContext *ctx, int async)
{

    if (ctx == NULL) {
        return;
    }
#if HAVE_LIBEVENT == 1
    if (async) {
        if (ctx->ev_base != NULL) {
            event_base_free(ctx->ev_base);
        }
    }
#endif
    SCFree(ctx);
}

/** \brief SCLogFileCloseRedis() Closes redis log more
 *  \param log_ctx Log file context allocated by caller
 */
static void SCLogFileCloseRedis(LogFileCtx *log_ctx)
{
    SCLogRedisContext * ctx = log_ctx->redis;

    if ( ctx == NULL) {
        return;
    }

    /* asynchronous */
    if (log_ctx->redis_setup.async) {
#if HAVE_LIBEVENT == 1
        if (ctx->async != NULL) {
            redisAsyncFree(ctx->async);
        }
        if (ctx->ev_base) {
            event_base_loopbreak(ctx->ev_base);
        }
        ctx->async = NULL;
#endif
    } else {

        /* synchronous */
        if (ctx->sync) {
            redisReply *reply;
            int i;
            for (i = 0; i < log_ctx->redis_setup.batch_count; i++) {
                redisGetReply(ctx->sync, (void **)&reply);
                if (reply)
                    freeReplyObject(reply);
            }
            redisFree(ctx->sync);
            ctx->sync = NULL;
        }
        log_ctx->redis_setup.tried = 0;
        log_ctx->redis_setup.batch_count = 0;
    }
}

static int SCConfLogReopenRedis(LogFileCtx *log_ctx);

#if HAVE_LIBEVENT == 1

#if HIREDIS_MAJOR == 0 && HIREDIS_MINOR < 11

/** \brief RedisConnectCallback() Closes redis log more
 *  \param c redis async context
 */
void RedisConnectCallback(const redisAsyncContext *c)
{
	SCLogInfo("Connected to redis server.");
}
#else

/** \brief RedisConnectCallback() Closes redis log more
 *  \param c redis async context
 *  \param status status reported by async caller
 */
void RedisConnectCallback(const redisAsyncContext *c, int status)
{
	SCLogInfo("Connected to redis server. Status [%d]", status);
}

#endif // HIREDIS_MAJOR == 0 && HIREDIS_MINOR < 11

/** \brief RedisDisconnectCallback() Callback when disconnection from redis happens.
 *  \param c redis async context
 *  \param status status reported by async caller
 */
void RedisDisconnectCallback(const redisAsyncContext *c, int status)
{
	SCLogInfo("Disconnected from redis server. Status [%d]", status);
}


/** \brief SCRedisAsyncCommandCallback() Callback when reply from redis happens.
 *  \param c redis async context
 *  \param r redis reply
 *  \param privvata opaque datq with pointer to LogFileCtx
 */
static void SCRedisAsyncCommandCallback (redisAsyncContext *c, void *r, void *privdata)
{

    LogFileCtx *file_ctx = privdata;
    SCLogRedisContext *ctx = file_ctx->redis;

    redisReply *reply = r;

    /* Disconnection or lost reply may have happened */
    if (reply == NULL) {
        if (ctx->ev_base != NULL) {
            event_base_loopbreak(ctx->ev_base);
        }
        SCConfLogReopenRedis(file_ctx);
        return;
    }

    if (reply->type == REDIS_REPLY_INTEGER ) {
        SCLogDebug("redis reply: %lld\n", reply->integer);
    }

    if (reply->type == REDIS_REPLY_STRING && reply->str) {
        SCLogDebug("redis reply: %s\n", reply->str);
    }

    redisAsyncHandleWrite(c);
}

#endif // HAVE_LIBEVENT == 1

/** \brief SCConfLogReopenRedis() Open or re-opens connection to redis for logging.
 *  \param log_ctx Log file context allocated by caller
 */
static int SCConfLogReopenRedis(LogFileCtx *log_ctx)
{
	/* only try to reconnect once per second */
	if (log_ctx->redis_setup.tried >= time(NULL)) {
		return -1;
	}

	SCLogRedisContext * ctx = log_ctx->redis;
	const char *redis_server = log_ctx->redis_setup.server;
	int redis_port = log_ctx->redis_setup.port;

#if HAVE_LIBEVENT == 1
    if (log_ctx->redis_setup.async) {

        if (ctx->async != NULL)  {
            redisAsyncDisconnect(ctx->async);
            redisAsyncFree(ctx->async);
        }

        ctx->async = NULL;

        /* ASYNC */
        redisAsyncContext *c = redisAsyncConnect(redis_server, redis_port);
        if ( c != NULL && c->err) {
            SCLogError(SC_ERR_SOCKET, "Error connecting to redis server: [%s] !", c->errstr);
            redisAsyncFree(c);
            ctx->async = NULL;
            log_ctx->redis_setup.tried = time(NULL);
            return -1;
        }
        if (c != NULL)  {
            redisLibeventAttach(c,ctx->ev_base);
            redisAsyncSetConnectCallback(c,RedisConnectCallback);
            redisAsyncSetDisconnectCallback(c,RedisDisconnectCallback);
            SCLogInfo("Connection to redis server [%s]:[%d] will use async.",
                    redis_server, redis_port);

            redisAsyncHandleWrite(c);

            ctx->async = c;
        }
    } else
#endif
	{
        /* SYNCHRONOUS */
        if (ctx->sync != NULL)  {
            redisFree(ctx->sync);
        }

        ctx->sync = NULL;

        redisContext *c = redisConnect(redis_server, redis_port);
        if ( c != NULL && c->err) {
            SCLogError(SC_ERR_SOCKET, "Error connecting to redis server: [%s] !", c->errstr);
            redisFree(c);
            ctx->sync = NULL;
            log_ctx->redis_setup.tried = time(NULL);
            return -1;
        }
        ctx->sync = c;
    }

    log_ctx->redis = ctx;
    log_ctx->redis_setup.tried = 0;
    log_ctx->redis_setup.batch_count = 0;
    return 0;
}


/** \brief configure and initializes redis output logginh
 *  \param conf ConfNode structure for the output section in question
 *  \param log_ctx Log file context allocated by caller
 *  \retval 0 on success
 */
int SCConfLogOpenRedis(ConfNode *redis_node, LogFileCtx *log_ctx)
{
    const char *redis_server = NULL;
    const char *redis_port = NULL;
    const char *redis_mode = NULL;
    const char *redis_key = NULL;
    int async = 0;

    if (redis_node) {
        redis_server = ConfNodeLookupChildValue(redis_node, "server");
        redis_port =  ConfNodeLookupChildValue(redis_node, "port");
        redis_mode =  ConfNodeLookupChildValue(redis_node, "mode");
        redis_key =  ConfNodeLookupChildValue(redis_node, "key");
    }
    if (!redis_server) {
        redis_server = "127.0.0.1";
        SCLogInfo("Using default redis server (127.0.0.1)");
    }
    if (!redis_port)
        redis_port = "6379";
    if (!redis_mode)
        redis_mode = "list";
    if (!redis_key)
        redis_key = "suricata";
    log_ctx->redis_setup.key = SCStrdup(redis_key);

    ConfGetChildValueBool(redis_node, "async", &async);
    log_ctx->redis_setup.async = async;

#if HAVE_LIBEVENT == 0
    if (async) {
        SCLogWarning(SC_ERR_NO_LIBEVENT, "Async option not available. Compile with --enable-libevent.");
    }
    log_ctx->redis_setup.async = 0;
#endif

    if (!log_ctx->redis_setup.key) {
        SCLogError(SC_ERR_MEM_ALLOC, "Unable to allocate redis key name");
        exit(EXIT_FAILURE);
    }

    log_ctx->redis_setup.batch_size = 0;

    ConfNode *pipelining = ConfNodeLookupChild(redis_node, "pipelining");
    if (pipelining) {
        int enabled = 0;
        int ret;
        intmax_t val;
        ret = ConfGetChildValueBool(pipelining, "enabled", &enabled);
        if (ret && enabled) {
            ret = ConfGetChildValueInt(pipelining, "batch-size", &val);
            if (ret) {
                log_ctx->redis_setup.batch_size = val;
            } else {
                log_ctx->redis_setup.batch_size = 10;
            }
        }
    }

    if (!strcmp(redis_mode, "list")) {
        log_ctx->redis_setup.command = redis_push_cmd;
        if (!log_ctx->redis_setup.command) {
            SCLogError(SC_ERR_MEM_ALLOC, "Unable to allocate redis key command");
            exit(EXIT_FAILURE);
        }
    } else {
        log_ctx->redis_setup.command = redis_publish_cmd;
        if (!log_ctx->redis_setup.command) {
            SCLogError(SC_ERR_MEM_ALLOC, "Unable to allocate redis key command!");
            exit(EXIT_FAILURE);
        }
    }


    /* store server params for reconnection */
    log_ctx->redis_setup.server = SCStrdup(redis_server);
    if (!log_ctx->redis_setup.server) {
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating redis server string !");
        exit(EXIT_FAILURE);
    }
    log_ctx->redis_setup.port = atoi(redis_port);
    log_ctx->redis_setup.tried = 0;

    log_ctx->redis = SCLogRedisContextAlloc(async);
    SCConfLogReopenRedis(log_ctx);
    log_ctx->Close = SCLogFileCloseRedis;

    return 0;
}

#endif

#ifdef HAVE_LIBRDKAFKA

static rd_kafka_conf_t * KafkaConfNew() {

    /* Kafka configuration */
    rd_kafka_conf_t *conf = rd_kafka_conf_new();
    if (!conf) {
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating kafka conf");
        exit(EXIT_FAILURE);
    }

    return conf;
}

static rd_kafka_conf_res_t KafkaConfSetInt(rd_kafka_conf_t *conf, const char * key, intmax_t value)
{
    char buf[21] = {0};
    uint32_t sz  = sizeof(buf);

    char errstr[2048]  = {0};
    uint32_t errstr_sz = sizeof(errstr);

    uint32_t offset = 0;
    PrintBufferData(buf, &offset, sz, "%lu", value);

    rd_kafka_conf_res_t ret = rd_kafka_conf_set(conf, key, buf, errstr, errstr_sz);
    if (ret != RD_KAFKA_CONF_OK) {
        SCLogWarning(SC_ERR_MEM_ALLOC, "Failed to set kafka conf [%s] => [%s] : %s", key, buf, errstr);
    }

    return ret;
}


static rd_kafka_conf_res_t KafkaConfSetString(rd_kafka_conf_t *conf, const char * key, const char *value)
{
    char errstr[2048]  = {0};
    uint32_t errstr_sz = sizeof(errstr);

    rd_kafka_conf_res_t ret = rd_kafka_conf_set(conf, key, value, errstr, errstr_sz);
    if(ret != RD_KAFKA_CONF_OK) {
        SCLogWarning(SC_ERR_MEM_ALLOC, "Failed to set kafka conf [%s] => [%s] : %s", key, value, errstr);
    }

    return ret;
}

static rd_kafka_conf_res_t KafkaTopicConfSetString(rd_kafka_topic_conf_t *conf, const char * key, const char *value)
{
    char errstr[2048]  = {0};
    uint32_t errstr_sz = sizeof(errstr);

    rd_kafka_conf_res_t ret = rd_kafka_topic_conf_set(conf, key, value, errstr, errstr_sz);
    if(ret != RD_KAFKA_CONF_OK) {
        SCLogWarning(SC_ERR_MEM_ALLOC, "Failed to set kafka topic conf [%s] => [%s] : %s", key, value, errstr);
    }

    return ret;
}

static rd_kafka_conf_t* KafkaConfSetup(rd_kafka_conf_t *conf, const char *sensor_name,
        const char *compression,
        intmax_t buffer_max_messages, intmax_t max_retries, intmax_t backoff_ms,
        intmax_t loglevel
        )
{

    /* Setting client id with sensor's name */
    KafkaConfSetString(conf, "client.id", sensor_name);

    /* Compression */
    KafkaConfSetString(conf, "compression.codec", compression);

    /* Configure throughput */
    KafkaConfSetInt(conf, "queue.buffering.max.messages", buffer_max_messages);

    /* Configure retries */
    KafkaConfSetInt(conf, "message.send.max.retries", max_retries);

    /* Configure backoff in ms */
    KafkaConfSetInt(conf, "retry.backoff.ms", backoff_ms);

    /* Configure debug sections */
    KafkaConfSetInt(conf, "log_level", loglevel);

    /* Configure debug sections */
    KafkaConfSetString(conf, "debug", "all");

    return conf;
}

static void SCLogFileCloseKafka(LogFileCtx *log_ctx)
{

    if (log_ctx->kafka_setup.brokers) {
        /* Destroy brokers */
        SCFree(log_ctx->kafka_setup.brokers);
        log_ctx->kafka_setup.brokers = NULL;
    }

    if (log_ctx->kafka_setup.topic) {
        /* Destroy topic */
        rd_kafka_topic_destroy(log_ctx->kafka_setup.topic);
        log_ctx->kafka_setup.topic = NULL;
    }

    if (log_ctx->kafka) {
        /* Destroy the handle */
        rd_kafka_destroy(log_ctx->kafka);
        log_ctx->kafka = NULL;
    }

}

static void KafkaLogCb(const rd_kafka_t *rk, int level, const char *fac, const char *buf)
{

    switch(level) {
        case SC_LOG_NOTSET:
        case SC_LOG_NONE:
            break;
        case SC_LOG_NOTICE:
            SCLogNotice("RDKAFKA-%i-%s: %s: %s\n", level, fac, rd_kafka_name(rk), buf);
            break;
        case SC_LOG_INFO:
            SCLogInfo("RDKAFKA-%i-%s: %s: %s\n", level, fac, rd_kafka_name(rk), buf);
            break;
        case SC_LOG_EMERGENCY:
            SCLogEmerg(SC_ERR_SOCKET,"RDKAFKA-%i-%s: %s: %s\n", level, fac, rd_kafka_name(rk), buf);
            break;
        case SC_LOG_CRITICAL:
            SCLogCritical(SC_ERR_SOCKET, "RDKAFKA-%i-%s: %s: %s\n", level, fac, rd_kafka_name(rk), buf);
            break;
        case SC_LOG_ALERT:
            SCLogAlert(SC_ERR_SOCKET, "RDKAFKA-%i-%s: %s: %s\n", level, fac, rd_kafka_name(rk), buf);
            break;
        case SC_LOG_ERROR:
            SCLogError(SC_ERR_SOCKET, "RDKAFKA-%i-%s: %s: %s\n", level, fac, rd_kafka_name(rk), buf);
            break;
        case SC_LOG_WARNING:
            SCLogWarning(SC_ERR_SOCKET, "RDKAFKA-%i-%s: %s: %s\n", level, fac, rd_kafka_name(rk), buf);
            break;
        case SC_LOG_DEBUG:
            SCLogDebug("RDKAFKA-%i-%s: %s: %s\n", level, fac, rd_kafka_name(rk), buf);
            break;
        default:
            /* OTHER LOG LEVELS */
            break;
    }
}

int SCConfLogOpenKafka(ConfNode *kafka_node, LogFileCtx *log_ctx)
{
    /* Kafka default values */
    const char *    kafka_default_broker_list         = "127.0.0.1:9092";
    const char *    kafka_default_compression         = "snappy";
    const char *    kafka_default_topic               = "suricata";
    const intmax_t  kafka_default_max_retries         = 1;
    const intmax_t  kafka_default_backoff_ms          = 10;
    const intmax_t  kafka_default_buffer_max_messages = 100000;
    const intmax_t  kafka_default_loglevel            = 6;
    const intmax_t  kafka_default_partition           = RD_KAFKA_PARTITION_UA; /* Unassigned partition */

    const char *brokers          = kafka_default_broker_list;
    const char *compression      = kafka_default_compression;
    const char *topic            = kafka_default_topic;
    intmax_t max_retries         = kafka_default_max_retries;
    intmax_t backoff_ms          = kafka_default_backoff_ms;
    intmax_t buffer_max_messages = kafka_default_buffer_max_messages;
    intmax_t loglevel            = kafka_default_loglevel;
    intmax_t partition           = 0;

    if (! kafka_node )
        return -1;

    brokers = ConfNodeLookupChildValue(kafka_node, "broker-list");

    if (! brokers) {
        brokers = kafka_default_broker_list;
        SCLogWarning(SC_ERR_MISSING_CONFIG_PARAM, "eve kafka output: using default broker: %s", kafka_default_broker_list);
    }

    compression = ConfNodeLookupChildValue(kafka_node, "compression");

    if (! compression) {
        compression = kafka_default_compression;
        SCLogInfo("eve kafka output: using default compression: %s", kafka_default_compression);
    }

    topic = ConfNodeLookupChildValue(kafka_node, "topic");
    if (! topic) {
        topic = kafka_default_topic;
        SCLogWarning(SC_ERR_MISSING_CONFIG_PARAM, "eve kafka output: using default topic: %s", kafka_default_topic);
    }

    if (! ConfGetChildValueInt(kafka_node, "max-retries", &max_retries) ) {
        SCLogInfo("eve kafka output: using default max-retries: %lu", kafka_default_max_retries);
    }

    if (! ConfGetChildValueInt(kafka_node, "backoff-ms", &backoff_ms) ) {
        SCLogInfo("eve kafka output: using default backoff-ms: %lu", kafka_default_backoff_ms);
    }

    if (! ConfGetChildValueInt(kafka_node, "buffer-max-messages", &buffer_max_messages) ) {
        SCLogInfo("eve kafka output: using default buffer-max-messages: %lu", kafka_default_buffer_max_messages);
    }

    if (! ConfGetChildValueInt(kafka_node, "partition", &partition) ) {
        SCLogInfo("eve kafka output: using default unassigned partition");
    }

    if (! ConfGetChildValueInt(kafka_node, "log-level", &loglevel) ) {
        SCLogInfo("eve kafka output: using default log-level: %lu", kafka_default_loglevel);
    } else {
        SCLogInfo("eve kafka output: log-level: %lu", loglevel);
    }

    log_ctx->kafka_setup.brokers   = SCStrdup(brokers);
    if (!log_ctx->kafka_setup.brokers) {
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating kafka brokers");
        exit(EXIT_FAILURE);
    }

    if (partition < 0) {
        partition = kafka_default_partition;
        SCLogInfo("eve kafka output: using default unassigned partition");
    }

    /* Configures and starts up kafka things */
    {
        char errstr[2048]  = {0};

        rd_kafka_t *rk                    = NULL;
        rd_kafka_topic_conf_t *topic_conf = NULL;
        rd_kafka_topic_t *rkt             = NULL;

        /* Check librdkafka version and emit warning if outside of tested versions */
        if ( RD_KAFKA_VERSION > 0x000901ff || RD_KAFKA_VERSION < 0x00080100 ) {
            SCLogWarning(SC_ERR_SOCKET, "librdkafka version check fails : %x", RD_KAFKA_VERSION);
        }

        /* Kafka configuration */
        rd_kafka_conf_t *conf = KafkaConfNew();

        /* Set configurations */
        conf = KafkaConfSetup(conf,
            log_ctx->sensor_name,
            compression, buffer_max_messages, max_retries, backoff_ms, loglevel);

        /* Set log callback */
        rd_kafka_conf_set_log_cb(conf, KafkaLogCb);

        /* Create Kafka handle */
        if (!(rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr)))) {
            SCLogError(SC_ERR_MEM_ALLOC, "Failed to create kafka handler: %s", errstr);
            exit(EXIT_FAILURE);
        }

        /* Set the log level */
        rd_kafka_set_log_level(rk, loglevel);

        /* Add brokers */
        if (rd_kafka_brokers_add(rk, brokers) == 0) {
            SCLogError(SC_ERR_MEM_ALLOC, "Failed to add kafka brokers: %s", brokers);
            exit(EXIT_FAILURE);
        } else {
            SCLogInfo("eve kafka output: afka brokers added: %s", brokers);
        }

        /* Topic configuration - Not saved at setup */
        if ( !(topic_conf = rd_kafka_topic_conf_new())) {
            SCLogError(SC_ERR_MEM_ALLOC, "Failed to allocate kafka topic conf");
            exit(EXIT_FAILURE);
        }

        /* Configure acks */
        KafkaTopicConfSetString(topic_conf, "request.required.acks", "0");

        /* Topic  */
        if ( !(rkt = rd_kafka_topic_new(rk, topic, topic_conf))) {
            SCLogError(SC_ERR_MEM_ALLOC, "Failed to allocate kafka topic %s", topic);
            exit(EXIT_FAILURE);
        }

        log_ctx->kafka                   = rk;
        log_ctx->kafka_setup.topic       = rkt;
        log_ctx->kafka_setup.conf        = conf;
        log_ctx->kafka_setup.loglevel    = loglevel;
        log_ctx->kafka_setup.partition   = partition;
        log_ctx->kafka_setup.tried       = 0;

        SCLogInfo("eve kafka ouput: handler ready and configured!");
    }

    log_ctx->Close = SCLogFileCloseKafka;
    return 0;
}

int SCConfLogReopenKafka(LogFileCtx *log_ctx)
{
    if (log_ctx->kafka != NULL) {
        rd_kafka_destroy(log_ctx->kafka);
        log_ctx->kafka = NULL;
    }

    // only try to reconnect once per second
    if (log_ctx->kafka_setup.tried >= time(NULL)) {
        return -1;
    }

    {
        rd_kafka_t *rk     = NULL;
        char errstr[2048]  = {0};

        /* Create Kafka handle */
        if (!(rk = rd_kafka_new(RD_KAFKA_PRODUCER, log_ctx->kafka_setup.conf, errstr, sizeof(errstr)))) {
            SCLogError(SC_ERR_SOCKET, "Failed to create kafka handler: %s", errstr);
            return -1;
        }

        rd_kafka_set_log_level(rk, log_ctx->kafka_setup.loglevel);

        log_ctx->kafka             = rk;
        log_ctx->kafka_setup.tried = 0;
    }

    return 0;
}
#endif

/** \brief LogFileNewCtx() Get a new LogFileCtx
 *  \retval LogFileCtx * pointer if succesful, NULL if error
 *  */
LogFileCtx *LogFileNewCtx(void)
{
    LogFileCtx* lf_ctx;
    lf_ctx = (LogFileCtx*)SCMalloc(sizeof(LogFileCtx));

    if (lf_ctx == NULL)
        return NULL;
    memset(lf_ctx, 0, sizeof(LogFileCtx));

    SCMutexInit(&lf_ctx->fp_mutex,NULL);

    // Default Write and Close functions
    lf_ctx->Write = SCLogFileWrite;
    lf_ctx->Close = SCLogFileClose;

#ifdef HAVE_LIBHIREDIS
    lf_ctx->redis_setup.batch_count = 0;
#endif

    return lf_ctx;
}

/** \brief LogFileFreeCtx() Destroy a LogFileCtx (Close the file and free memory)
 *  \param motcx pointer to the OutputCtx
 *  \retval int 1 if succesful, 0 if error
 *  */
int LogFileFreeCtx(LogFileCtx *lf_ctx)
{
    if (lf_ctx == NULL) {
        SCReturnInt(0);
    }

    if (lf_ctx->fp != NULL) {
        SCMutexLock(&lf_ctx->fp_mutex);
        lf_ctx->Close(lf_ctx);
        SCMutexUnlock(&lf_ctx->fp_mutex);
    }

#ifdef HAVE_LIBHIREDIS
    if (lf_ctx->type == LOGFILE_TYPE_REDIS) {
        if (lf_ctx->redis_setup.server)
            SCFree(lf_ctx->redis_setup.server);
        if (lf_ctx->redis_setup.key)
            SCFree(lf_ctx->redis_setup.key);
        if (lf_ctx->redis)
            SCLogRedisContextFree(lf_ctx->redis, lf_ctx->redis_setup.async);
    }
#endif
#ifdef HAVE_LIBRDKAFKA

    if (lf_ctx->type == LOGFILE_TYPE_KAFKA) {
        SCMutexLock(&lf_ctx->fp_mutex);
        SCLogFileCloseKafka(lf_ctx);
        SCMutexUnlock(&lf_ctx->fp_mutex);
    }
#endif

    SCMutexDestroy(&lf_ctx->fp_mutex);

    if (lf_ctx->prefix != NULL) {
        SCFree(lf_ctx->prefix);
        lf_ctx->prefix_len = 0;
    }

    if(lf_ctx->filename != NULL)
        SCFree(lf_ctx->filename);

    if (lf_ctx->sensor_name)
        SCFree(lf_ctx->sensor_name);

    OutputUnregisterFileRotationFlag(&lf_ctx->rotation_flag);

    SCFree(lf_ctx);

    SCReturnInt(1);
}

#ifdef HAVE_LIBHIREDIS

/**
 * \brief LogFileWriteRedis() writes log data to redis output.
 * \param log_ctx Log file context allocated by caller
 * \param string buffer with data to write
 * \param string_len data length
 * \retval 0 on sucess;
 * \retval -1 on failure;
 */
static int LogFileWriteRedis(LogFileCtx *file_ctx, const char *string, size_t string_len)
{
    if (file_ctx->redis == NULL) {
        SCConfLogReopenRedis(file_ctx);
        if (file_ctx->redis == NULL) {
            return -1;
        } else {
            SCLogInfo("Reconnected to redis server.");
        }
    }

	SCLogRedisContext * ctx = file_ctx->redis;

#if HAVE_LIBEVENT == 1
	/* async mode on */
    if (file_ctx->redis_setup.async) {

        redisAsyncContext * redis_async = ctx->async;

        if (redis_async) {
            SCLogDebug("redis async command: %s", file_ctx->redis_setup.command);
            redisAsyncCommand(redis_async,
                    SCRedisAsyncCommandCallback,
                    file_ctx,
                    "%s %s %s",
                    file_ctx->redis_setup.command,
                    file_ctx->redis_setup.key,
                    string);

            redisAsyncHandleWrite(redis_async);
            redisAsyncHandleRead(redis_async);

        }
        return 0;
    }
#endif

    redisContext *redis = ctx->sync;

    if (unlikely(redis == NULL)) {
        SCConfLogReopenRedis(file_ctx);
    }

    /* synchronous mode */
    if (file_ctx->redis_setup.batch_size) {

        redisAppendCommand(redis, "%s %s %s",
                file_ctx->redis_setup.command,
                file_ctx->redis_setup.key,
                string);
        if (file_ctx->redis_setup.batch_count == file_ctx->redis_setup.batch_size) {
            redisReply *reply;
            int i;
            file_ctx->redis_setup.batch_count = 0;
            for (i = 0; i <= file_ctx->redis_setup.batch_size; i++) {
                if (redisGetReply(redis, (void **)&reply) == REDIS_OK) {
                    freeReplyObject(reply);
                } else {
                    if (redis->err) {
                        SCLogInfo("Error when fetching reply: %s (%d)",
                                redis->errstr,
                                redis->err);
                    }
                    switch (redis->err) {
                        case REDIS_ERR_EOF:
                        case REDIS_ERR_IO:
                            SCLogInfo("Reopening connection to redis server");
                            SCConfLogReopenRedis(file_ctx);
                            if (file_ctx->redis) {
                                SCLogInfo("Reconnected to redis server");
                                return 0;
                            } else {
                                SCLogInfo("Unable to reconnect to redis server");
                                return 0;
                            }
                            break;
                        default:
                            SCLogWarning(SC_ERR_INVALID_VALUE,
                                    "Unsupported error code %d",
                                    redis->err);
                            return 0;
                    }
                }
            }
        } else {
            file_ctx->redis_setup.batch_count++;
        }
    } else {
        redisReply *reply = redisCommand(redis, "%s %s %s",
                file_ctx->redis_setup.command,
                file_ctx->redis_setup.key,
                string);

        /* We may lose the reply if disconnection happens*/
        if (reply)  {

            switch (reply->type) {
                case REDIS_REPLY_ERROR:
                    SCLogWarning(SC_ERR_SOCKET, "Redis error: %s", reply->str);
                    SCConfLogReopenRedis(file_ctx);
                    break;
                case REDIS_REPLY_INTEGER:
                    SCLogDebug("Redis integer %lld", reply->integer);
                    break;
                default:
                    SCLogError(SC_ERR_INVALID_VALUE,
                            "Redis default triggered with %d", reply->type);
                    SCConfLogReopenRedis(file_ctx);
                    break;
            }

            freeReplyObject(reply);
        }  else {
            if (unlikely(reply == NULL)) {
                SCConfLogReopenRedis(file_ctx);
            }
        }
    }
    return 0;
}
#endif

#ifdef HAVE_LIBRDKAFKA
static int LogFileWriteKafka(LogFileCtx *file_ctx, const char *string, size_t string_len)
{
    rd_kafka_t *rk = file_ctx->kafka;

    if (rk == NULL) {
        SCConfLogReopenKafka(file_ctx);
        if (rk == NULL) {
            SCLogInfo("Connection to kafka brokers not possible.");
            return -1;
        } else {
            SCLogInfo("Reconnected to Kafka brokers.");
        }
    }

    int err = -1;

    /* Send/Produce message. */
    if ((err =  rd_kafka_produce(
                    file_ctx->kafka_setup.topic,
                    file_ctx->kafka_setup.partition,
                    RD_KAFKA_MSG_F_COPY,
                    /* Payload and length */
                    (char *)string, string_len,
                    /* Optional key and its length */
                    NULL, 0,
                    /* Message opaque, provided in
                     * delivery report callback as
                     * msg_opaque. */
                    NULL)) == -1) {

        const char *errstr = rd_kafka_err2str(rd_kafka_errno2err(err));

        SCLogError(SC_ERR_SOCKET,
                "%% Failed to produce to topic %s "
                "partition %i: %s\n",
                rd_kafka_topic_name(file_ctx->kafka_setup.topic),
                file_ctx->kafka_setup.partition,
                errstr);
    } else {
        SCLogDebug("KAFKA MSG:[%s] ERR:[%d] QUEUE:[%d]", string, err, rd_kafka_outq_len(rk));
    }


    return 0;
}
#endif

int LogFileWrite(LogFileCtx *file_ctx, MemBuffer *buffer)
{

    if (file_ctx->type == LOGFILE_TYPE_SYSLOG) {
        syslog(file_ctx->syslog_setup.alert_syslog_level, "%s",
                (const char *)MEMBUFFER_BUFFER(buffer));
    } else if (file_ctx->type == LOGFILE_TYPE_FILE ||
               file_ctx->type == LOGFILE_TYPE_UNIX_DGRAM ||
               file_ctx->type == LOGFILE_TYPE_UNIX_STREAM)
    {
        /* append \n for files only */
        MemBufferWriteString(buffer, "\n");
        SCMutexLock(&file_ctx->fp_mutex);
        file_ctx->Write((const char *)MEMBUFFER_BUFFER(buffer),
                        MEMBUFFER_OFFSET(buffer), file_ctx);
        SCMutexUnlock(&file_ctx->fp_mutex);
    }
#ifdef HAVE_LIBHIREDIS
    else if (file_ctx->type == LOGFILE_TYPE_REDIS) {
        SCMutexLock(&file_ctx->fp_mutex);
        LogFileWriteRedis(file_ctx, (const char *)MEMBUFFER_BUFFER(buffer),
                MEMBUFFER_OFFSET(buffer));
        SCMutexUnlock(&file_ctx->fp_mutex);
    }
#endif

#ifdef HAVE_LIBRDKAFKA
    else if (file_ctx->type == LOGFILE_TYPE_KAFKA) {
        SCMutexLock(&file_ctx->fp_mutex);
        LogFileWriteKafka(file_ctx, (const char *)MEMBUFFER_BUFFER(buffer),
                MEMBUFFER_OFFSET(buffer));
        SCMutexUnlock(&file_ctx->fp_mutex);
    }
#endif

    return 0;
}

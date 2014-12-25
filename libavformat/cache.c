/*
 * Input cache protocol.
 * Copyright (c) 2011,2014 Michael Niedermayer
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * Based on file.c by Fabrice Bellard
 */

/**
 * @TODO
 *      support keeping files
 *      support filling with a background thread
 */

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/file.h"
#include "libavutil/tree.h"
#include "avformat.h"
#include <fcntl.h>
#if HAVE_IO_H
#include <io.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/stat.h>
#include <stdlib.h>
#include "os_support.h"
#include "url.h"

typedef struct CacheEntry {
    int64_t logical_pos;
    int64_t physical_pos;
    int size;
} CacheEntry;

typedef struct Context {
    int fd;
    struct AVTreeNode *root;
    int64_t logical_pos;
    int64_t cache_pos;
    int64_t inner_pos;
    int64_t end;
    int is_true_eof;
    URLContext *inner;
    int64_t cache_hit, cache_miss;
} Context;

static int cmp(void *key, const void *node)
{
    return (*(int64_t *) key) - ((const CacheEntry *) node)->logical_pos;
}

static int cache_open(URLContext *h, const char *arg, int flags)
{
    char *buffername;
    Context *c= h->priv_data;

    av_strstart(arg, "cache:", &arg);

    c->fd = av_tempfile("ffcache", &buffername, 0, h);
    if (c->fd < 0){
        av_log(h, AV_LOG_ERROR, "Failed to create tempfile\n");
        return c->fd;
    }

    unlink(buffername);
    av_freep(&buffername);

    return ffurl_open(&c->inner, arg, flags, &h->interrupt_callback, NULL);
}

static int add_entry(URLContext *h, const unsigned char *buf, int size)
{
    Context *c= h->priv_data;
    int64_t pos;
    int ret;
    CacheEntry *entry = av_malloc(sizeof(*entry));
    CacheEntry *entry_ret;
    struct AVTreeNode *node = av_tree_node_alloc();

    if (!entry || !node) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    //FIXME avoid lseek
    pos = lseek(c->fd, 0, SEEK_END);
    if (pos < 0) {
        ret = AVERROR(errno);
        av_log(h, AV_LOG_ERROR, "seek in cache failed\n");
        goto fail;
    }

    ret = write(c->fd, buf, size);
    if (ret < 0) {
        ret = AVERROR(errno);
        av_log(h, AV_LOG_ERROR, "write in cache failed\n");
        goto fail;
    }

    entry->logical_pos = c->logical_pos;
    entry->physical_pos = pos;
    entry->size = ret;

    entry_ret = av_tree_insert(&c->root, entry, cmp, &node);
    if (entry_ret && entry_ret != entry) {
        ret = -1;
        av_log(h, AV_LOG_ERROR, "av_tree_insert failed\n");
        goto fail;
    }
    c->cache_pos = entry->physical_pos + entry->size;

    return 0;
fail:
    av_free(entry);
    av_free(node);
    return ret;
}

static int cache_read(URLContext *h, unsigned char *buf, int size)
{
    Context *c= h->priv_data;
    CacheEntry *entry, *next[2] = {NULL, NULL};
    int r;

    entry = av_tree_find(c->root, &c->logical_pos, cmp, (void**)next);

    if (!entry)
        entry = next[0];

    if (entry) {
        int64_t in_block_pos = c->logical_pos - entry->logical_pos;
        av_assert0(entry->logical_pos <= c->logical_pos);
        if (in_block_pos < entry->size) {
            int64_t physical_target = entry->physical_pos + in_block_pos;
            //FIXME avoid seek if unneeded
            r = lseek(c->fd, physical_target, SEEK_SET);
            if (r >= 0)
                r = read(c->fd, buf, FFMIN(size, entry->size - in_block_pos));

            if (r > 0) {
                c->logical_pos += r;
                c->cache_hit ++;
                return r;
            }
        }
    }

    // Cache miss or some kind of fault with the cache

    if (c->logical_pos != c->inner_pos) {
        r = ffurl_seek(c->inner, c->logical_pos, SEEK_SET);
        if (r<0) {
            av_log(h, AV_LOG_ERROR, "Failed to perform internal seek\n");
            return r;
        }
        c->inner_pos = r;
    }

    r = ffurl_read(c->inner, buf, size);
    if (r == 0 && size>0) {
        c->is_true_eof = 1;
        av_assert0(c->end >= c->logical_pos);
    }
    if (r<=0)
        return r;
    c->inner_pos += r;

    c->cache_miss ++;

    add_entry(h, buf, r);
    c->logical_pos += r;
    c->end = FFMAX(c->end, c->logical_pos);

    return r;
}

static int64_t cache_seek(URLContext *h, int64_t pos, int whence)
{
    Context *c= h->priv_data;

    if (whence == AVSEEK_SIZE) {
        pos= ffurl_seek(c->inner, pos, whence);
        if(pos <= 0){
            pos= ffurl_seek(c->inner, -1, SEEK_END);
            if (ffurl_seek(c->inner, c->inner_pos, SEEK_SET) < 0)
                av_log(h, AV_LOG_ERROR, "Inner protocol failed to seekback end : %"PRId64"\n", pos);
        }
        if (pos > 0)
            c->is_true_eof = 1;
        c->end = FFMAX(c->end, pos);
        return pos;
    }

    if (whence == SEEK_CUR) {
        whence = SEEK_SET;
        pos += c->logical_pos;
    } else if (whence == SEEK_END && c->is_true_eof) {
        whence = SEEK_SET;
        pos += c->end;
    }

    if (whence == SEEK_SET && pos >= 0 && pos < c->end) {
        //Seems within filesize, assume it will not fail.
        c->logical_pos = pos;
        return pos;
    }

    //cache miss
    pos = lseek(c->fd, pos, whence);
    if (pos >= 0) {
        c->logical_pos = pos;
        c->end = FFMAX(c->end, pos);
    }

    return pos;
}

static int cache_close(URLContext *h)
{
    Context *c= h->priv_data;

    av_log(h, AV_LOG_INFO, "Statistics, cache hits:%"PRId64" cache misses:%"PRId64"\n",
           c->cache_hit, c->cache_miss);

    close(c->fd);
    ffurl_close(c->inner);
    av_tree_destroy(c->root);


    return 0;
}

URLProtocol ff_cache_protocol = {
    .name                = "cache",
    .url_open            = cache_open,
    .url_read            = cache_read,
    .url_seek            = cache_seek,
    .url_close           = cache_close,
    .priv_data_size      = sizeof(Context),
};

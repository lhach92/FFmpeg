/*
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
 */

#include <stdint.h>
#include <string.h>

#include "atomic.h"
#include "buffer_internal.h"
#include "common.h"
#include "mem.h"

AVBufferRef *av_buffer_create(uint8_t *data, int size,
                              void (*free)(void *opaque, uint8_t *data),
                              void *opaque, int flags)
{
    AVBufferRef *ref = NULL;
    AVBuffer    *buf = NULL;

    buf = av_mallocz(sizeof(*buf));
    if (!buf)
        return NULL;

    buf->data     = data;
    buf->size     = size;
    buf->free     = free ? free : av_buffer_default_free;
    buf->opaque   = opaque;
    buf->refcount = 1;

    if (flags & AV_BUFFER_FLAG_READONLY)
        buf->flags |= BUFFER_FLAG_READONLY;

    ref = av_mallocz(sizeof(*ref));
    if (!ref) {
        av_freep(&buf);
        return NULL;
    }

    ref->buffer = buf;
    ref->data   = data;
    ref->size   = size;

    return ref;
}

void av_buffer_default_free(void *opaque, uint8_t *data)
{
    av_free(data);
}

AVBufferRef *av_buffer_alloc(int size)
{
    AVBufferRef *ret = NULL;
    uint8_t    *data = NULL;

    data = av_malloc(size);
    if (!data)
        return NULL;

    if(CONFIG_MEMORY_POISONING)
        memset(data, 0x2a, size);

    ret = av_buffer_create(data, size, av_buffer_default_free, NULL, 0);
    if (!ret)
        av_freep(&data);

    return ret;
}

AVBufferRef *av_buffer_allocz(int size)
{
    AVBufferRef *ret = av_buffer_alloc(size);
    if (!ret)
        return NULL;

    memset(ret->data, 0, size);
    return ret;
}

AVBufferRef *av_buffer_ref(AVBufferRef *buf)
{
    AVBufferRef *ret = av_mallocz(sizeof(*ret));

    if (!ret)
        return NULL;

    *ret = *buf;

    avpriv_atomic_int_add_and_fetch(&buf->buffer->refcount, 1);

    return ret;
}

void av_buffer_unref(AVBufferRef **buf)
{
    AVBuffer *b;

    if (!buf || !*buf)
        return;
    b = (*buf)->buffer;
    av_freep(buf);

    if (!avpriv_atomic_int_add_and_fetch(&b->refcount, -1)) {
        b->free(b->opaque, b->data);
        av_freep(&b);
    }
}

int av_buffer_is_writable(const AVBufferRef *buf)
{
    if (buf->buffer->flags & AV_BUFFER_FLAG_READONLY)
        return 0;

    return avpriv_atomic_int_get(&buf->buffer->refcount) == 1;
}

int av_buffer_make_writable(AVBufferRef **pbuf)
{
    AVBufferRef *newbuf, *buf = *pbuf;

    if (av_buffer_is_writable(buf))
        return 0;

    newbuf = av_buffer_alloc(buf->size);
    if (!newbuf)
        return AVERROR(ENOMEM);

    memcpy(newbuf->data, buf->data, buf->size);
    av_buffer_unref(pbuf);
    *pbuf = newbuf;

    return 0;
}

int av_buffer_realloc(AVBufferRef **pbuf, int size)
{
    AVBufferRef *buf = *pbuf;
    uint8_t *tmp;

    if (!buf) {
        /* allocate a new buffer with av_realloc(), so it will be reallocatable
         * later */
        uint8_t *data = av_realloc(NULL, size);
        if (!data)
            return AVERROR(ENOMEM);

        buf = av_buffer_create(data, size, av_buffer_default_free, NULL, 0);
        if (!buf) {
            av_freep(&data);
            return AVERROR(ENOMEM);
        }

        buf->buffer->flags |= BUFFER_FLAG_REALLOCATABLE;
        *pbuf = buf;

        return 0;
    } else if (buf->size == size)
        return 0;

    if (!(buf->buffer->flags & BUFFER_FLAG_REALLOCATABLE) ||
        !av_buffer_is_writable(buf)) {
        /* cannot realloc, allocate a new reallocable buffer and copy data */
        AVBufferRef *new = NULL;

        av_buffer_realloc(&new, size);
        if (!new)
            return AVERROR(ENOMEM);

        memcpy(new->data, buf->data, FFMIN(size, buf->size));

        av_buffer_unref(pbuf);
        *pbuf = new;
        return 0;
    }

    tmp = av_realloc(buf->buffer->data, size);
    if (!tmp)
        return AVERROR(ENOMEM);

    buf->buffer->data = buf->data = tmp;
    buf->buffer->size = buf->size = size;
    return 0;
}

AVBufferPool *av_buffer_pool_init(int size, AVBufferRef* (*alloc)(int size))
{
    AVBufferPool *pool = av_mallocz(sizeof(*pool));
    if (!pool)
        return NULL;

    pool->size     = size;
    pool->alloc    = alloc ? alloc : av_buffer_alloc;

    avpriv_atomic_int_set(&pool->refcount, 1);

    return pool;
}

/*
 * This function gets called when the pool has been uninited and
 * all the buffers returned to it.
 */
static void buffer_pool_free(AVBufferPool *pool)
{
    while (pool->pool) {
        BufferPoolEntry *buf = pool->pool;
        pool->pool = buf->next;

        buf->free(buf->opaque, buf->data);
        av_freep(&buf);
    }
    av_freep(&pool);
}

void av_buffer_pool_uninit(AVBufferPool **ppool)
{
    AVBufferPool *pool;

    if (!ppool || !*ppool)
        return;
    pool   = *ppool;
    *ppool = NULL;

    if (!avpriv_atomic_int_add_and_fetch(&pool->refcount, -1))
        buffer_pool_free(pool);
}

/* remove the whole buffer list from the pool and return it */
static BufferPoolEntry *get_pool(AVBufferPool *pool)
{
    BufferPoolEntry *cur = *(void * volatile *)&pool->pool, *last = NULL;

    while (cur != last) {
        last = cur;
        cur = avpriv_atomic_ptr_cas((void * volatile *)&pool->pool, last, NULL);
        if (!cur)
            return NULL;
    }

    return cur;
}

static void add_to_pool(BufferPoolEntry *buf)
{
    AVBufferPool *pool;
    BufferPoolEntry *cur, *end = buf;

    if (!buf)
        return;
    pool = buf->pool;

    while (end->next)
        end = end->next;

    while (avpriv_atomic_ptr_cas((void * volatile *)&pool->pool, NULL, buf)) {
        /* pool is not empty, retrieve it and append it to our list */
        cur = get_pool(pool);
        end->next = cur;
        while (end->next)
            end = end->next;
    }
}

static void pool_release_buffer(void *opaque, uint8_t *data)
{
    BufferPoolEntry *buf = opaque;
    AVBufferPool *pool = buf->pool;

    if(CONFIG_MEMORY_POISONING)
        memset(buf->data, 0x2a, pool->size);

    add_to_pool(buf);
    if (!avpriv_atomic_int_add_and_fetch(&pool->refcount, -1))
        buffer_pool_free(pool);
}

/* allocate a new buffer and override its free() callback so that
 * it is returned to the pool on free */
static AVBufferRef *pool_alloc_buffer(AVBufferPool *pool)
{
    BufferPoolEntry *buf;
    AVBufferRef     *ret;

    ret = pool->alloc(pool->size);
    if (!ret)
        return NULL;

    buf = av_mallocz(sizeof(*buf));
    if (!buf) {
        av_buffer_unref(&ret);
        return NULL;
    }

    buf->data   = ret->buffer->data;
    buf->opaque = ret->buffer->opaque;
    buf->free   = ret->buffer->free;
    buf->pool   = pool;

    ret->buffer->opaque = buf;
    ret->buffer->free   = pool_release_buffer;

    avpriv_atomic_int_add_and_fetch(&pool->refcount, 1);
    avpriv_atomic_int_add_and_fetch(&pool->nb_allocated, 1);

    return ret;
}

AVBufferRef *av_buffer_pool_get(AVBufferPool *pool)
{
    AVBufferRef *ret;
    BufferPoolEntry *buf;

    /* check whether the pool is empty */
    buf = get_pool(pool);
    if (!buf && pool->refcount <= pool->nb_allocated) {
        av_log(NULL, AV_LOG_DEBUG, "Pool race dectected, spining to avoid overallocation and eventual OOM\n");
        while (!buf && avpriv_atomic_int_get(&pool->refcount) <= avpriv_atomic_int_get(&pool->nb_allocated))
            buf = get_pool(pool);
    }

    if (!buf)
        return pool_alloc_buffer(pool);

    /* keep the first entry, return the rest of the list to the pool */
    add_to_pool(buf->next);
    buf->next = NULL;

    ret = av_buffer_create(buf->data, pool->size, pool_release_buffer,
                           buf, 0);
    if (!ret) {
        add_to_pool(buf);
        return NULL;
    }
    avpriv_atomic_int_add_and_fetch(&pool->refcount, 1);

    return ret;
}

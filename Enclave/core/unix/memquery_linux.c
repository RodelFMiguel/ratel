/* *******************************************************************************
 * Copyright (c) 2010-2017 Google, Inc.  All rights reserved.
 * Copyright (c) 2011 Massachusetts Institute of Technology  All rights reserved.
 * Copyright (c) 2000-2010 VMware, Inc.  All rights reserved.
 * *******************************************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of VMware, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* Copyright (c) 2003-2007 Determina Corp. */
/* Copyright (c) 2001-2003 Massachusetts Institute of Technology */
/* Copyright (c) 2000-2001 Hewlett-Packard Company */

/*
 * memquery_linux.c - memory querying via /proc/self/maps
 */

#include "../globals.h"
#include "memquery.h"
#include "os_private.h"
#include "module_private.h"
#include <string.h>
#include <sys/mman.h>
#include "sgx_vma.h"

#ifndef LINUX
# error Linux-only
#endif

/* Iterator over /proc/self/maps
 * Called at arbitrary places, so cannot use fopen.
 * The whole thing is serialized, so entries cannot be referenced
 * once the iteration ends.
 */

/* Internal data */
typedef struct _maps_iter_t {
    file_t maps;
    char *newline;
    int bufread;
    int bufwant;
    char *buf;
    char *comment_buffer;
} maps_iter_t;

/* lock to guard reads from /proc/self/maps in get_memory_info_from_os(). */
static mutex_t memory_info_buf_lock = INIT_LOCK_FREE(memory_info_buf_lock);
/* lock for iterator where user needs to allocate memory */
static mutex_t maps_iter_buf_lock = INIT_LOCK_FREE(maps_iter_buf_lock);

/* On all supported linux kernels /proc/self/maps -> /proc/$pid/maps
 * However, what we want is /proc/$tid/maps, so we can't use "self"
 */
#define PROC_SELF_MAPS "/proc/self/maps"

/* these are defined in /usr/src/linux/fs/proc/array.c */
#define MAPS_LINE_LENGTH        4096
/* for systems with sizeof(void*) == 4: */
#define MAPS_LINE_FORMAT4  "%08lx-%08lx %s %08lx %*s "UINT64_FORMAT_STRING" %4096[^\n]"
#define MAPS_LINE_MAX4  49 /* sum of 8  1  8  1 4 1 8 1 5 1 10 1 */
/* for systems with sizeof(void*) == 8: */
#define MAPS_LINE_FORMAT8  "%016lx-%016lx %s %016lx %*s "UINT64_FORMAT_STRING" %4096[^\n]"
#define MAPS_LINE_MAX8  73 /* sum of 16  1  16  1 4 1 16 1 5 1 10 1 */

#define MAPS_LINE_MAX   MAPS_LINE_MAX8

/* can't use fopen -- strategy: read into buffer, look for newlines.
 * fail if single line too large for buffer -- so size it appropriately:
 */
/* since we're called from signal handler, etc., keep our stack usage
 * low by using static bufs (it's over 4K after all).
 * FIXME: now we're using 16K right here: should we shrink?
 */
#define BUFSIZE (MAPS_LINE_LENGTH+8)
static char buf_scratch[BUFSIZE];
static char comment_buf_scratch[BUFSIZE];
/* To satisfy our two uses (inner use with memory_info_buf_lock versus
 * outer use with maps_iter_buf_lock), we have two different locks and
 * two different sets of static buffers.  This is to avoid lock
 * ordering issues: we need an inner lock for use in places like signal
 * handlers, but an outer lock when the iterator user allocates memory.
 */
static char buf_iter[BUFSIZE];
static char comment_buf_iter[BUFSIZE];


#include "instrument_api.h"

#define USING_SGX_PROCMAPS

#define SGX_PROCMAPS_BUF_LEN   (4096*2)
char sgx_procmaps_buf[SGX_PROCMAPS_BUF_LEN];

struct procmaps_t {
    char*   buf;        /* point ro the current buffer; */
    size_t  buf_size;   /* the size of buffer */
    size_t  cnt_len;    /* length of content */
    size_t  read_oft;   /* offset from begining for reading */
} sgx_procmaps;


// extern byte* sgx_vm_base;
extern byte* heap_init_end;

/* print the vma list to a buffer */
int sgx_procmaps_read_start(void)
{
    sgx_procmaps.buf = sgx_procmaps_buf;
    sgx_procmaps.buf_size = SGX_PROCMAPS_BUF_LEN;
    sgx_procmaps.cnt_len = 0;
    sgx_procmaps.read_oft = 0;

    sgx_vm_area_t *vma;
    list_t* ll;
    char perm[8];
    byte* start;
    byte* end;

    char *pbuf;
    char* pcnt;
    size_t nleft;
    size_t nwrite;

#define MAPS_LINE_FORMAT  "%08lx-%08lx %s %08lx %-8d %-8d %8s\n"
#define AFTER_HEAP_FLAG   (byte*)0x800000000000
#define SGX_BUFFER_SIZE   0x000010000000

    for (ll = sgxmm.in.next; ll != &sgxmm.in; ll = ll->next) {
        vma = list_entry(ll, sgx_vm_area_t, ll);

        /* Please don't expose the sgx-mm-buffer itself */
        // if (vma->vm_start >= sgx_vm_base && vma->vm_end <= sgx_vm_base + SGX_BUFFER_SIZE)
        // if (vma->vm_start >= SGX_VM_HEAPBASE && vma->vm_end <= (SGX_VM_HEAPBASE + SGX_BUFFER_SIZE))
        if (vma->vm_end == heap_init_end)
            continue;

        /* TCS is not accessile to Application, making it non-visible such that would not be emulated */
        if (strncmp(vma->comment, "[TCS]", 80) == 0)
            continue;

        /* perm to string */
        strncpy(perm, "---p", 8);
        if (vma->perm & PROT_READ)
            perm[0] = 'r';
        if (vma->perm & PROT_WRITE)
            perm[1] = 'w';
        if (vma->perm & PROT_EXEC)
            perm[2] = 'x';

        /* check the buffer available for wrtting */
        nleft = sgx_procmaps.buf_size - sgx_procmaps.cnt_len;
        if (nleft < 256) {
            /* dynamically allocate a big buffer */
            sgx_procmaps.buf_size *= 2;

            pbuf = (char*)heap_alloc(GLOBAL_DCONTEXT, sgx_procmaps.buf_size HEAPACCT(ACCT_OTHER));
            YPHASSERT(pbuf != NULL);

            memcpy(pbuf, sgx_procmaps.buf, sgx_procmaps.cnt_len);

            /* free the old buffer */
            if (sgx_procmaps.buf != sgx_procmaps_buf)
                heap_free(GLOBAL_DCONTEXT, sgx_procmaps.buf, sgx_procmaps.buf_size HEAPACCT(ACCT_OTHER));

            /* update the fields of sgx_procmaps */
            sgx_procmaps.buf = pbuf;
            nleft = sgx_procmaps.buf_size - sgx_procmaps.cnt_len;
        }

        pcnt = sgx_procmaps.buf + sgx_procmaps.cnt_len;
        start = sgx_mm_ext2itn(vma->vm_start);
        if (sgx_mm_within(start, vma->vm_end - vma->vm_start)) {
            YPHASSERT (vma->vm_sgx != NULL);
            end = start + (vma->vm_end - vma->vm_start);
        }
        else if (start > AFTER_HEAP_FLAG) {
            start = (byte*)(vma->vm_start - AFTER_HEAP_FLAG);
            end = (byte*)(vma->vm_end - AFTER_HEAP_FLAG);
        }
        else {
            start = vma->vm_start;
            end = vma->vm_end;
        }
        nwrite = snprintf(pcnt, nleft, MAPS_LINE_FORMAT, start, end, perm, vma->offset, vma->dev, vma->inode, vma->comment);
        dr_printf(MAPS_LINE_FORMAT, start, end, perm, vma->offset, vma->dev, vma->inode, vma->comment);

        sgx_procmaps.cnt_len += nwrite;
    }

    return !INVALID_FILE;
}


void sgx_procmaps_read_stop(void)
{   /* exit querying mode */
    if (sgx_procmaps.buf != sgx_procmaps_buf) {
        heap_free(GLOBAL_DCONTEXT, sgx_procmaps.buf, sgx_procmaps.buf_size HEAPACCT(ACCT_OTHER));
    }
    sgx_procmaps.cnt_len = sgx_procmaps.read_oft = 0;
}


/* There is a synchronization problem: the vma list may be updated during querying */
/* To simulate querying the procmaps, We make sure the caller always see the old memory layout */
int sgx_procmaps_read_next(char *buf, int count)
{
    char* pcnt;
    size_t ncnt;

    if (sgx_procmaps.read_oft >= sgx_procmaps.cnt_len) {
        return 0;
    }

    pcnt = sgx_procmaps.buf + sgx_procmaps.read_oft;
    ncnt = sgx_procmaps.cnt_len - sgx_procmaps.read_oft;
    if (ncnt > count) {
        memcpy(buf, pcnt, count);
        sgx_procmaps.read_oft += count;

        return count;
    }
    else {
        memcpy(buf, pcnt, ncnt);
        buf[ncnt] = '\0';
        sgx_procmaps.read_oft = sgx_procmaps.cnt_len;

        return ncnt;
    }
}


void
memquery_init(void)
{
    /* XXX: if anything substantial is added here, the memquery use
     * in privload_early_inject() will have to be re-evaluated.
     */
    ASSERT(sizeof(maps_iter_t) <= MEMQUERY_INTERNAL_DATA_LEN);
}

void
memquery_exit(void)
{
    DELETE_LOCK(memory_info_buf_lock);
    DELETE_LOCK(maps_iter_buf_lock);
}

bool
memquery_from_os_will_block(void)
{
#ifdef DEADLOCK_AVOIDANCE
    return memory_info_buf_lock.owner != INVALID_THREAD_ID;
#else
    /* "may_alloc" is false for memquery_from_os() */
    bool res = true;
    if (mutex_trylock(&memory_info_buf_lock)) {
        res = false;
        mutex_unlock(&memory_info_buf_lock);
    }
    return res;
#endif
}

bool
memquery_iterator_start(memquery_iter_t *iter, app_pc start, bool may_alloc)
{
#ifndef USING_SGX_PROCMAPS
    char maps_name[24]; /* should only need 16 for 5-digit tid */
#endif
    maps_iter_t *mi = (maps_iter_t *) &iter->internal;

    /* Don't assign the local ptrs until the lock is grabbed to make
     * their relationship clear. */
    if (may_alloc) {
        mutex_lock(&maps_iter_buf_lock);
        mi->buf = (char *) &buf_iter;
        mi->comment_buffer = (char *) &comment_buf_iter;
    } else {
        mutex_lock(&memory_info_buf_lock);
        mi->buf = (char *) &buf_scratch;
        mi->comment_buffer = (char *) &comment_buf_scratch;
    }

    /* We need the maps for our thread id, not process id.
     * "/proc/self/maps" uses pid which fails if primary thread in group
     * has exited.
     */
#ifndef USING_SGX_PROCMAPS
    snprintf(maps_name, BUFFER_SIZE_ELEMENTS(maps_name),
             "/proc/%d/maps", get_thread_id());
    mi->maps = os_open(maps_name, OS_OPEN_READ);
    ASSERT(mi->maps != INVALID_FILE);
#else
    sgx_procmaps_read_start();
#endif
    mi->buf[BUFSIZE-1] = '\0'; /* permanently */

    mi->newline = NULL;
    mi->bufread = 0;
    iter->comment = mi->comment_buffer;

    iter->may_alloc = may_alloc;

    /* XXX: it's quite difficult to start at the region containing "start":
     * we either need to support walking backward a line (complicated by
     * the incremental os_read() scheme) or make two passes.
     * Thus, the interface doesn't promise we'll start there.
     */
    iter->vm_start = NULL;

    return true;
}

void
memquery_iterator_stop(memquery_iter_t *iter)
{
    ASSERT((iter->may_alloc && OWN_MUTEX(&maps_iter_buf_lock)) ||
           (!iter->may_alloc && OWN_MUTEX(&memory_info_buf_lock)));
#ifndef USING_SGX_PROCMAPS
    maps_iter_t *mi = (maps_iter_t *) &iter->internal;
    os_close(mi->maps);
#else
    sgx_procmaps_read_stop();
#endif
    if (iter->may_alloc)
        mutex_unlock(&maps_iter_buf_lock);
    else
        mutex_unlock(&memory_info_buf_lock);
}

bool
memquery_iterator_next(memquery_iter_t *iter)
{
    maps_iter_t *mi = (maps_iter_t *) &iter->internal;
    char perm[16];
    char *line;
    int len;
    app_pc prev_start = iter->vm_start;
    ASSERT((iter->may_alloc && OWN_MUTEX(&maps_iter_buf_lock)) ||
           (!iter->may_alloc && OWN_MUTEX(&memory_info_buf_lock)));
    if (mi->newline == NULL) {
        mi->bufwant = BUFSIZE-1;
#ifndef USING_SGX_PROCMAPS
        mi->bufread = os_read(mi->maps, mi->buf, mi->bufwant);
#else
        mi->bufread = sgx_procmaps_read_next(mi->buf, mi->bufwant);
#endif
        ASSERT(mi->bufread <= mi->bufwant);
        LOG(GLOBAL, LOG_VMAREAS, 6,
            "get_memory_info_from_os: bytes read %d/want %d\n",
            mi->bufread, mi->bufwant);
        if (mi->bufread <= 0)
            return false;
        mi->buf[mi->bufread] = '\0';
        mi->newline = strchr(mi->buf, '\n');
        line = mi->buf;
    } else {
        line = mi->newline + 1;
        mi->newline = strchr(line, '\n');
        if (mi->newline == NULL) {
            /* FIXME clean up: factor out repetitive code */
            /* shift 1st part of line to start of buf, then read in rest */
            /* the memory for the processed part can be reused  */
            mi->bufwant = line - mi->buf;
            ASSERT(mi->bufwant <= mi->bufread);
            len = mi->bufread - mi->bufwant; /* what is left from last time */
            /* since strings may overlap, should use memmove, not strncpy */
            /* FIXME corner case: if len == 0, nothing to move */
            memmove(mi->buf, line, len);
#ifndef USING_SGX_PROCMAPS
            mi->bufread = os_read(mi->maps, mi->buf+len, mi->bufwant);
#else
            mi->bufread = sgx_procmaps_read_next(mi->buf+len, mi->bufwant);
#endif
            ASSERT(mi->bufread <= mi->bufwant);
            if (mi->bufread <= 0)
                return false;
            mi->bufread += len; /* bufread is total in buf */
            mi->buf[mi->bufread] = '\0';
            mi->newline = strchr(mi->buf, '\n');
            line = mi->buf;
        }
    }
    LOG(GLOBAL, LOG_VMAREAS, 6,
        "\nget_memory_info_from_os: newline=[%s]\n",
        mi->newline ? mi->newline : "(null)");

    /* Buffer is big enough to hold at least one line: if not, the file changed
     * underneath us after we hit the end.  Just bail.
     */
    if (mi->newline == NULL)
        return false;
    *mi->newline = '\0';
    LOG(GLOBAL, LOG_VMAREAS, 6,
        "\nget_memory_info_from_os: line=[%s]\n", line);
    mi->comment_buffer[0]='\0';
    len = sscanf(line,
#ifdef IA32_ON_IA64
                 MAPS_LINE_FORMAT8, /* cross-compiling! */
#else
                 sizeof(void*) == 4 ? MAPS_LINE_FORMAT4 : MAPS_LINE_FORMAT8,
#endif
                 (unsigned long*)&iter->vm_start, (unsigned long*)&iter->vm_end,
                 perm, (unsigned long*)&iter->offset, &iter->inode,
                 mi->comment_buffer);
    if (iter->vm_start == iter->vm_end) {
        /* i#366 & i#599: Merge an empty regions caused by stack guard pages
         * into the stack region if the stack region is less than one page away.
         * Otherwise skip it.  Some Linux kernels (2.6.32 has been observed)
         * have empty entries for the stack guard page.  We drop the permissions
         * on the guard page, because Linux always insists that it has rwxp
         * perms, no matter how we change the protections.  The actual stack
         * region has the perms we expect.
         * XXX: We could get more accurate info if we looked at
         * /proc/self/smaps, which has a Size: 4k line for these "empty"
         * regions.
         */
        app_pc empty_start = iter->vm_start;
        bool r;
        LOG(GLOBAL, LOG_VMAREAS, 2,
            "maps_iterator_next: skipping or merging empty region 0x%08x\n",
            iter->vm_start);
        /* don't trigger the maps-file-changed check.
         * slight risk of a race where we'll pass back earlier/overlapping
         * region: we'll live with it.
         */
        iter->vm_start = NULL;
        r = memquery_iterator_next(iter);
        /* We could check to see if we're combining with the [stack] section,
         * but that doesn't work if there are multiple stacks or the stack is
         * split into multiple maps entries, so we merge any empty region within
         * one page of the next region.
         */
        if (empty_start <= iter->vm_start &&
            iter->vm_start <= empty_start + PAGE_SIZE) {
            /* Merge regions if the next region was zero or one page away. */
            iter->vm_start = empty_start;
        }
        return r;
    }
    if (iter->vm_start <= prev_start) {
        /* the maps file has expanded underneath us (presumably due to our
         * own committing while iterating): skip ahead */
        LOG(GLOBAL, LOG_VMAREAS, 2,
            "maps_iterator_next: maps file changed: skipping 0x%08x\n", prev_start);
        iter->vm_start = prev_start;
        return memquery_iterator_next(iter);
    }
    if (len<6)
        mi->comment_buffer[0]='\0';
    iter->prot = permstr_to_memprot(perm);
#ifdef ANDROID
    /* i#1861: the Android kernel supports custom comments which can't merge */
    if (iter->comment[0] != '\0')
        iter->prot |= MEMPROT_HAS_COMMENT;
#endif
    return true;
}

/***************************************************************************
 * LIBRARY BOUNDS
 */

/* See memquery.h for full interface specs.
 * Gets the library bounds from walking the map file (as opposed to using our cached
 * module_list) since is only used for dr and client libraries which aren't on the list.
 */
int
memquery_library_bounds(const char *name, app_pc *start/*IN/OUT*/, app_pc *end/*OUT*/,
                        char *fullpath/*OPTIONAL OUT*/, size_t path_size)
{
    return memquery_library_bounds_by_iterator(name, start, end, fullpath, path_size);
}

/***************************************************************************
 * QUERY
 */

bool
memquery_from_os(const byte *pc, OUT dr_mem_info_t *info, OUT bool *have_type)
{
    memquery_iter_t iter;
    app_pc last_end = NULL;
    app_pc next_start = (app_pc) POINTER_MAX;
    bool found = false;
    ASSERT(info != NULL);
    memquery_iterator_start(&iter, (app_pc) pc, false/*won't alloc*/);
    while (memquery_iterator_next(&iter)) {
        if (pc >= iter.vm_start && pc < iter.vm_end) {
            info->base_pc = iter.vm_start;
            info->size = (iter.vm_end - iter.vm_start);
            info->prot = iter.prot;
            /* On early (pre-Fedora 2) kernels the vsyscall page is listed
             * with no permissions at all in the maps file.  Here's RHEL4:
             *   ffffe000-fffff000 ---p 00000000 00:00 0
             * We return "rx" as the permissions in that case.
             */
            if (vsyscall_page_start != NULL &&
                pc >= vsyscall_page_start && pc < vsyscall_page_start+PAGE_SIZE) {
                /* i#1583: recent kernels have 2-page vdso, which can be split,
                 * but we don't expect to come here b/c they won't have zero
                 * permissions.
                 */
                ASSERT(iter.vm_start == vsyscall_page_start);
                ASSERT(iter.vm_end - iter.vm_start == PAGE_SIZE ||
                       /* i386 Ubuntu 14.04:
                        * 0xb77bc000-0xb77be000   0x2000    0x0 [vvar]
                        * 0xb77be000-0xb77c0000   0x2000    0x0 [vdso]
                        */
                       iter.vm_end - iter.vm_start == 2*PAGE_SIZE);
                info->prot = (MEMPROT_READ|MEMPROT_EXEC|MEMPROT_VDSO);
            } else if (strcmp(iter.comment, "[vvar]") == 0) {
                /* The VVAR pages were added in kernel 3.0 but not labeled until
                 * 3.15.  We document that we do not label prior to 3.15.
                 * DrMem#1778 seems to only happen on 3.19+ in any case.
                 */
                info->prot |= MEMPROT_VDSO;
            }
            found = true;
            break;
        } else if (pc < iter.vm_start) {
            next_start = iter.vm_start;
            break;
        }
        last_end = iter.vm_end;
    }
    memquery_iterator_stop(&iter);
    if (!found) {
        info->base_pc = last_end;
        info->size = (next_start - last_end);
        info->prot = MEMPROT_NONE;
        info->type = DR_MEMTYPE_FREE;
        *have_type = true;
    }
    return true;
}

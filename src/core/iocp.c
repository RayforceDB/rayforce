/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "core/platform.h"

#if defined(RAY_OS_WINDOWS)

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

#include "core/poll.h"
#include "core/mcast.h"
#include "core/timer.h"
#include "mem/sys.h"
#include "mem/heap.h"   /* idle decay: bound the wait, sweep after wakeup */

/* Windows event loop.
 *
 * Readiness-based, like the epoll and kqueue backends, so the selector
 * state machine (rx fill -> read_fn -> data_fn, tx flush) is the same on
 * every platform.  Sockets are waited on with WSAPoll.  Standard input is
 * not a socket and WSAPoll cannot watch it, so RAY_SEL_STDIN selectors are
 * probed directly (console input queue, bytes in a pipe) and the socket
 * wait is cut into short slices while one is registered.
 *
 * WSAPoll keeps no kernel-side registration, so the wait set is rebuilt
 * from poll->sels on every pass: register/deregister and tx request/cancel
 * need no OS call, and a selector's write interest simply follows whether
 * it has a pending tx buffer. */

#define RAY_POLL_INITIAL_CAP    16
#define RAY_POLL_STDIN_SLICE_MS 10

enum { EV_IN = 1, EV_OUT = 2, EV_HUP = 4 };

/* ===== stdin readiness ===== */

/* Would a read of `fd` return now (data or EOF) instead of blocking? */
static int stdin_events(int64_t fd)
{
    HANDLE h = (HANDLE)_get_osfhandle((int)fd);
    if (h == INVALID_HANDLE_VALUE) return EV_HUP;

    switch (GetFileType(h)) {
    case FILE_TYPE_CHAR: {
        DWORD mode;
        if (!GetConsoleMode(h, &mode)) return EV_IN;   /* NUL device: never blocks */
        for (;;) {
            INPUT_RECORD rec;
            DWORD n = 0;
            if (!PeekConsoleInputW(h, &rec, 1, &n)) return EV_HUP;
            if (n == 0) return 0;
            if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown &&
                rec.Event.KeyEvent.uChar.UnicodeChar != 0)
                return EV_IN;
            /* Focus, mouse, resize and key-up records never yield a byte to
             * ReadFile: drop them, or a "ready" console would make the
             * reader block in ReadFile until the next real keystroke. */
            if (!ReadConsoleInputW(h, &rec, 1, &n)) return EV_HUP;
        }
    }
    case FILE_TYPE_PIPE: {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
            return EV_IN | EV_HUP;   /* writer gone: the read returns EOF */
        return avail ? EV_IN : 0;
    }
    default:
        return EV_IN;                /* disk file: reads never block */
    }
}

/* ===== Lifecycle ===== */

ray_poll_t* ray_poll_create(void)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NULL;

    ray_poll_t* poll = (ray_poll_t*)ray_sys_alloc(sizeof(ray_poll_t));
    if (!poll) return NULL;

    memset(poll, 0, sizeof(*poll));
    poll->fd      = -1;            /* no kernel object behind a WSAPoll loop */
    poll->code    = -1;
    poll->sel_cap = RAY_POLL_INITIAL_CAP;
    poll->sels    = (ray_selector_t**)ray_sys_alloc(
                        poll->sel_cap * sizeof(ray_selector_t*));
    if (!poll->sels) {
        ray_sys_free(poll);
        return NULL;
    }
    memset(poll->sels, 0, poll->sel_cap * sizeof(ray_selector_t*));
    return poll;
}

void ray_poll_destroy(ray_poll_t* poll)
{
    if (!poll) return;

    for (uint32_t i = 0; i < poll->n_sels; i++) {
        ray_selector_t* sel = poll->sels[i];
        if (!sel) continue;
        if (sel->close_fn) sel->close_fn(poll, sel);
        if (sel->rx.buf) ray_poll_buf_free(sel->rx.buf);
        ray_poll_buf_free(sel->tx.buf);
        ray_sys_free(sel);
        poll->sels[i] = NULL;
    }

    if (poll->sels) ray_sys_free(poll->sels);
    if (poll->timers) {
        ray_timers_destroy((ray_timers_t*)poll->timers);
        poll->timers = NULL;
    }
    if (poll->mcast) {
        ray_mcast_destroy((ray_mcast_t*)poll->mcast);
        poll->mcast = NULL;
    }
    ray_sys_free(poll);
}

/* ===== Registration ===== */

int64_t ray_poll_register(ray_poll_t* poll, ray_poll_reg_t* reg)
{
    if (!poll || !reg) return -1;

    /* Find free slot or grow */
    int64_t id = -1;
    for (uint32_t i = 0; i < poll->n_sels; i++) {
        if (!poll->sels[i]) { id = (int64_t)i; break; }
    }
    if (id < 0) {
        if (poll->n_sels >= poll->sel_cap) {
            uint32_t new_cap = poll->sel_cap * 2;
            ray_selector_t** ns = (ray_selector_t**)ray_sys_alloc(
                new_cap * sizeof(ray_selector_t*));
            if (!ns) return -1;
            memcpy(ns, poll->sels, poll->n_sels * sizeof(ray_selector_t*));
            memset(ns + poll->n_sels, 0,
                   (new_cap - poll->n_sels) * sizeof(ray_selector_t*));
            ray_sys_free(poll->sels);
            poll->sels    = ns;
            poll->sel_cap = new_cap;
        }
        id = (int64_t)poll->n_sels;
        poll->n_sels++;
    }

    ray_selector_t* sel = (ray_selector_t*)ray_sys_alloc(sizeof(ray_selector_t));
    if (!sel) return -1;
    memset(sel, 0, sizeof(*sel));

    sel->fd       = reg->fd;
    sel->id       = id;
    sel->type     = reg->type;
    sel->data     = reg->data;
    sel->open_fn  = reg->open_fn;
    sel->close_fn = reg->close_fn;
    sel->error_fn = reg->error_fn;
    sel->data_fn  = reg->data_fn;
    sel->rx.recv_fn = reg->recv_fn;
    sel->rx.read_fn = reg->read_fn;
    sel->tx.send_fn = reg->send_fn;

    poll->sels[id] = sel;
    poll->n_live++;
    if (sel->open_fn) sel->open_fn(poll, sel);
    return id;
}

/* Write interest is derived from sel->tx.buf when the wait set is built. */
void ray_poll_tx_request(ray_poll_t* poll, ray_selector_t* sel)
{
    (void)poll; (void)sel;
}

void ray_poll_tx_cancel(ray_poll_t* poll, ray_selector_t* sel)
{
    (void)poll; (void)sel;
}

void ray_poll_deregister(ray_poll_t* poll, int64_t id)
{
    if (!poll || id < 0 || (uint32_t)id >= poll->n_sels) return;
    ray_selector_t* sel = poll->sels[id];
    if (!sel) return;

    if (sel->close_fn) sel->close_fn(poll, sel);
    if (sel->rx.buf) ray_poll_buf_free(sel->rx.buf);
    ray_poll_buf_free(sel->tx.buf);
    ray_sys_free(sel);
    poll->sels[id] = NULL;
    if (poll->n_live > 0) poll->n_live--;
}

/* ===== Event dispatch (same order and rules as the epoll backend) ===== */

static void poll_dispatch(ray_poll_t* poll, uint64_t eid, int events)
{
    ray_selector_t* sel = NULL;
    if (eid < poll->n_sels)
        sel = poll->sels[eid];
    if (!sel) return;

    /* Process readable data first — even if hangup is also set.  A client
     * may send a message and close; both arrive in the same wakeup. */
    if (events & EV_IN) {
        /* Loop: read data -> call read_fn -> if state advanced, read more.
         * Handles multi-phase protocols (handshake -> header -> payload)
         * arriving in a single wakeup. */
        for (;;) {
            if (sel->rx.recv_fn && sel->rx.buf) {
                while (sel->rx.buf->offset < sel->rx.buf->size) {
                    int64_t nr = sel->rx.recv_fn(
                        sel->fd,
                        sel->rx.buf->data + sel->rx.buf->offset,
                        sel->rx.buf->size - sel->rx.buf->offset);
                    if (nr <= 0) {
                        if (nr < 0 && errno == EINTR) continue;
                        if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                            break;
                        /* Error or peer closed mid-read */
                        if (sel->error_fn)
                            sel->error_fn(poll, sel);
                        else
                            ray_poll_deregister(poll, sel->id);
                        return;
                    }
                    sel->rx.buf->offset += nr;
                }
            }

            /* Not enough data for current phase */
            if (sel->rx.buf && sel->rx.buf->offset < sel->rx.buf->size)
                break;

            /* Call read_fn — may advance state and request new buffer */
            if (!sel->rx.read_fn) break;
            ray_t* obj = sel->rx.read_fn(poll, sel);

            /* Re-validate: read_fn may have deregistered this selector */
            if (eid >= poll->n_sels || !poll->sels[eid]) return;
            sel = poll->sels[eid];

            if (obj && sel->data_fn)
                sel->data_fn(poll, sel, obj);

            if (eid >= poll->n_sels || !poll->sels[eid]) return;
            sel = poll->sels[eid];

            /* If no rx buffer (state machine done or not set), stop */
            if (!sel->rx.buf) break;
            /* If buffer already has enough data for next phase, loop */
            if (sel->rx.buf->offset >= sel->rx.buf->size) continue;
            /* Otherwise try reading more (may EAGAIN -> break) */
        }
    }

    if (events & EV_OUT) {
        if (eid >= poll->n_sels || !poll->sels[eid]) return;
        sel = poll->sels[eid];
        if (sel->tx.buf && ray_poll_tx_flush(poll, sel) < 0) {
            if (eid < poll->n_sels && poll->sels[eid]) {
                sel = poll->sels[eid];
                if (sel->error_fn)
                    sel->error_fn(poll, sel);
                else
                    ray_poll_deregister(poll, sel->id);
            }
            return;
        }
    }

    /* Error / hangup — after data is drained */
    if (events & EV_HUP) {
        if (eid < poll->n_sels && poll->sels[eid]) {
            sel = poll->sels[eid];
            if (sel->error_fn)
                sel->error_fn(poll, sel);
            else
                ray_poll_deregister(poll, sel->id);
        }
    }
}

/* ===== Run loop ===== */

int64_t ray_poll_run_for(ray_poll_t* poll, int timeout_ms)
{
    if (!poll) return -1;

    bool bounded = timeout_ms >= 0;
    int64_t end_ms = bounded ? ray_time_now_ms() + timeout_ms : INT64_MAX;

    while (poll->code < 0) {
        int wait_ms = -1;
        if (bounded) {
            int64_t remaining = end_ms - ray_time_now_ms();
            if (remaining < 0) remaining = 0;
            if (remaining > INT_MAX) remaining = INT_MAX;
            wait_ms = (int)remaining;
        }

        /* Nothing registered and nothing scheduled: an unbounded loop
         * would block here forever.  Return instead, so a process that
         * stayed only for its timers can end once they are spent. */
        if (!bounded && ray_poll_idle(poll)) return 0;

        if (poll->timers) {
            int64_t deadline = ray_timers_next_deadline_ms(
                (ray_timers_t*)poll->timers);
            if (deadline != INT64_MAX) {
                int64_t delta = deadline - ray_time_now_ms();
                if (delta < 0) delta = 0;
                if (delta > INT_MAX) delta = INT_MAX;
                if (wait_ms < 0 || delta < wait_ms)
                    wait_ms = (int)delta;
            }
        }

        /* Idle allocator decay bounds an unbounded wait, exactly as in the
         * epoll backend (see the comment there). */
        {
            int64_t decay = bounded ? -1 : ray_heap_decay_due_ms();
            if (decay >= 0) {
                if (decay > INT_MAX) decay = INT_MAX;
                if (wait_ms < 0 || decay < wait_ms) wait_ms = (int)decay;
            }
        }

        /* Build this pass's wait set: sockets for WSAPoll, stdin probed. */
        uint32_t   cap  = poll->n_sels ? poll->n_sels : 1;
        WSAPOLLFD* pfds = (WSAPOLLFD*)ray_sys_alloc(cap * sizeof(WSAPOLLFD));
        uint32_t*  pids = (uint32_t*)ray_sys_alloc(cap * sizeof(uint32_t));
        uint32_t*  sids = (uint32_t*)ray_sys_alloc(cap * sizeof(uint32_t));
        int*       sevs = (int*)ray_sys_alloc(cap * sizeof(int));
        if (!pfds || !pids || !sids || !sevs) {
            if (pfds) ray_sys_free(pfds);
            if (pids) ray_sys_free(pids);
            if (sids) ray_sys_free(sids);
            if (sevs) ray_sys_free(sevs);
            return -1;
        }

        uint32_t nfds = 0, nstd = 0;
        for (uint32_t i = 0; i < poll->n_sels; i++) {
            ray_selector_t* sel = poll->sels[i];
            if (!sel) continue;
            if (sel->type == RAY_SEL_STDIN) {
                sids[nstd] = i;
                sevs[nstd] = 0;
                nstd++;
                continue;
            }
            pfds[nfds].fd      = (SOCKET)sel->fd;
            pfds[nfds].events  = POLLRDNORM | (sel->tx.buf ? POLLWRNORM : 0);
            pfds[nfds].revents = 0;
            pids[nfds] = i;
            nfds++;
        }

        /* Wait.  With stdin registered the socket wait is sliced so the
         * console/pipe is re-probed every RAY_POLL_STDIN_SLICE_MS. */
        int64_t wait_end = wait_ms < 0 ? INT64_MAX : ray_time_now_ms() + wait_ms;
        int n = 0;
        bool failed = false;
        for (;;) {
            bool std_ready = false;
            for (uint32_t k = 0; k < nstd; k++) {
                sevs[k] = stdin_events(poll->sels[sids[k]]->fd);
                if (sevs[k]) std_ready = true;
            }

            int remain = -1;
            if (wait_end != INT64_MAX) {
                int64_t r = wait_end - ray_time_now_ms();
                remain = r < 0 ? 0 : (r > INT_MAX ? INT_MAX : (int)r);
            }
            int slice = std_ready ? 0 : remain;
            if (nstd && !std_ready &&
                (slice < 0 || slice > RAY_POLL_STDIN_SLICE_MS))
                slice = RAY_POLL_STDIN_SLICE_MS;

            if (nfds) {
                n = WSAPoll(pfds, nfds, slice);
                if (n == SOCKET_ERROR) {
                    if (WSAGetLastError() == WSAEINTR) continue;
                    failed = true;
                    break;
                }
            } else if (slice > 0) {
                Sleep((DWORD)slice);
            } else if (slice < 0) {
                /* Only reachable with a live selector that is neither a
                 * socket nor stdin; nothing can wake us, so just yield. */
                Sleep(RAY_POLL_STDIN_SLICE_MS);
            }

            if (n > 0 || std_ready) break;
            if (remain == 0) break;              /* deadline reached */
            if (!nstd && slice == remain) break; /* whole wait elapsed */
        }

        if (!failed) {
            for (uint32_t j = 0; j < nfds && n > 0; j++) {
                short re = pfds[j].revents;
                if (!re) continue;
                int ev = 0;
                if (re & (POLLRDNORM | POLLRDBAND))     ev |= EV_IN;
                if (re & POLLWRNORM)                    ev |= EV_OUT;
                if (re & (POLLERR | POLLHUP | POLLNVAL)) ev |= EV_HUP;
                /* The slot may have been reused by an earlier dispatch in
                 * this pass (deregister + accept): only act on the socket
                 * the event was reported for. */
                ray_selector_t* sel = pids[j] < poll->n_sels ? poll->sels[pids[j]] : NULL;
                if (!sel || (SOCKET)sel->fd != pfds[j].fd) continue;
                poll_dispatch(poll, pids[j], ev);
            }
            for (uint32_t k = 0; k < nstd; k++) {
                if (!sevs[k]) continue;
                ray_selector_t* sel = sids[k] < poll->n_sels ? poll->sels[sids[k]] : NULL;
                if (!sel || sel->type != RAY_SEL_STDIN) continue;
                poll_dispatch(poll, sids[k], sevs[k]);
            }
        }

        ray_sys_free(pfds);
        ray_sys_free(pids);
        ray_sys_free(sids);
        ray_sys_free(sevs);
        if (failed) return -1;

        if (poll->timers) {
            if (ray_timers_fire_expired((ray_timers_t*)poll->timers) > 0)
                ray_heap_note_activity();
        }
        ray_heap_decay();
        if (bounded) break;
    }

    return poll->code >= 0 ? poll->code : 0;
}

int64_t ray_poll_run(ray_poll_t* poll)
{
    return ray_poll_run_for(poll, -1);
}

#endif /* RAY_OS_WINDOWS */

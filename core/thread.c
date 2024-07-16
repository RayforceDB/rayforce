/*
 *   Copyright (c) 2024 Anton Kundenko <singaraiona@gmail.com>
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

#include "thread.h"
#include "util.h"

#if defined(OS_WINDOWS)

// Mutex functions
mutex_t mutex_create()
{
    mutex_t m;
    InitializeCriticalSection(&m.inner);
    return m;
}

nil_t mutex_destroy(mutex_t *mutex)
{
    DeleteCriticalSection(&mutex->inner);
}

nil_t mutex_lock(mutex_t *mutex)
{
    EnterCriticalSection(&mutex->inner);
}

nil_t mutex_unlock(mutex_t *mutex)
{
    LeaveCriticalSection(&mutex->inner);
}

// Condition variable functions
cond_t cond_create()
{
    cond_t c;
    InitializeConditionVariable(&c.inner);
    return c;
}

nil_t cond_destroy(cond_t *cond)
{
    // Windows does not require destruction of condition variables
    unused(cond);
}

i32_t cond_wait(cond_t *cond, mutex_t *mutex)
{
    return SleepConditionVariableCS(&cond->inner, &mutex->inner, INFINITE) ? 0 : -1;
}

int cond_signal(cond_t *cond)
{
    WakeConditionVariable(&cond->inner);
    return 0;
}

int cond_broadcast(cond_t *cond)
{
    WakeAllConditionVariable(&cond->inner);
    return 0;
}

// Thread functions
thread_t thread_create(void *(*fn)(void *), void *arg)
{
    thread_t t;
    t.handle = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)fn, arg, 0, NULL);
    return t;
}

i32_t thread_destroy(thread_t *thread)
{
    return CloseHandle(thread->handle) ? 0 : -1;
}

i32_t thread_join(thread_t thread)
{
    return WaitForSingleObject(thread.handle, INFINITE) == WAIT_OBJECT_0 ? 0 : -1;
}

i32_t thread_detach(thread_t thread)
{
    return CloseHandle(thread.handle) ? 0 : -1;
}

nil_t thread_exit(void *res)
{
    ExitThread((DWORD)res);
}

#else

mutex_t mutex_create()
{
    mutex_t mutex;
    pthread_mutex_init(&mutex.inner, NULL);
    return mutex;
}

nil_t mutex_destroy(mutex_t *mutex)
{
    pthread_mutex_destroy(&mutex->inner);
}

nil_t mutex_lock(mutex_t *mutex)
{
    pthread_mutex_lock(&mutex->inner);
}

nil_t mutex_unlock(mutex_t *mutex)
{
    pthread_mutex_unlock(&mutex->inner);
}

cond_t cond_create()
{
    cond_t cond;
    pthread_cond_init(&cond.inner, NULL);
    return cond;
}

nil_t cond_destroy(cond_t *cond)
{
    pthread_cond_destroy(&cond->inner);
}

i32_t cond_wait(cond_t *cond, mutex_t *mutex)
{
    return pthread_cond_wait(&cond->inner, &mutex->inner);
}

i32_t cond_signal(cond_t *cond)
{
    return pthread_cond_signal(&cond->inner);
}

i32_t cond_broadcast(cond_t *cond)
{
    return pthread_cond_broadcast(&cond->inner);
}

thread_t thread_create(raw_p (*fn)(raw_p), raw_p arg)
{
    thread_t thread;
    pthread_create(&thread.handle, NULL, fn, arg);
    return thread;
}

i32_t thread_destroy(thread_t *thread)
{
    return pthread_cancel(thread->handle);
}

i32_t thread_join(thread_t thread)
{
    return pthread_join(thread.handle, NULL);
}

i32_t thread_detach(thread_t thread)
{
    return pthread_detach(thread.handle);
}

nil_t thread_exit(raw_p res)
{
    return pthread_exit(res);
}

#endif

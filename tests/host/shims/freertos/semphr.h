#pragma once
#include "freertos/FreeRTOS.h"
#include <pthread.h>
#include <stdlib.h>

/* A SemaphoreHandle_t is a counting semaphore around a pthread mutex
 * + cond variable. We support both binary semaphores (Created via
 * xSemaphoreCreateBinary) and mutexes (xSemaphoreCreateMutex) — same
 * impl, different default state. */
struct host_sema {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    int             count;
};

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct host_sema *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->count = 1;                           /* mutex starts unlocked */
    return s;
}

static inline SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    struct host_sema *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->count = 0;                           /* binary starts empty */
    return s;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t to)
{
    pthread_mutex_lock(&s->mtx);
    if (to == portMAX_DELAY) {
        while (s->count == 0) pthread_cond_wait(&s->cv, &s->mtx);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  +=  to / 1000;
        ts.tv_nsec += (to % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        while (s->count == 0) {
            int r = pthread_cond_timedwait(&s->cv, &s->mtx, &ts);
            if (r == ETIMEDOUT) { pthread_mutex_unlock(&s->mtx); return pdFALSE; }
        }
    }
    s->count--;
    pthread_mutex_unlock(&s->mtx);
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    pthread_mutex_lock(&s->mtx);
    s->count++;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mtx);
    return pdTRUE;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t s)
{
    if (!s) return;
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->mtx);
    free(s);
}

#pragma once
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

typedef int                   BaseType_t;
typedef uint32_t              TickType_t;
typedef struct host_sema     *SemaphoreHandle_t;
typedef struct host_task     *TaskHandle_t;
typedef int                   portMUX_TYPE;

#define pdPASS               1
#define pdTRUE               1
#define pdFALSE              0
#define portMAX_DELAY        ((TickType_t)0xffffffffu)
#define portMUX_INITIALIZER_UNLOCKED 0

static inline TickType_t pdMS_TO_TICKS(uint32_t ms) { return ms; }

/* Critical sections become no-ops on host. */
#define portENTER_CRITICAL(mux)        ((void)(mux))
#define portEXIT_CRITICAL(mux)         ((void)(mux))
#define portENTER_CRITICAL_ISR(mux)    ((void)(mux))
#define portEXIT_CRITICAL_ISR(mux)     ((void)(mux))

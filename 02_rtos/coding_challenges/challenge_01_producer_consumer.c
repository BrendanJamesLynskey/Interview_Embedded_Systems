/*
 * Challenge 01: Producer-Consumer with FreeRTOS Queues
 *
 * Problem:
 *   Implement a producer-consumer pipeline using FreeRTOS message queues.
 *
 *   Producer tasks sample an ADC channel every 10 ms and send the reading to
 *   a shared queue.  A single consumer task receives readings, applies a
 *   simple moving-average filter, and outputs the filtered value.
 *
 *   Two producer tasks run concurrently (simulating two ADC channels).
 *   The consumer must handle readings from both channels correctly.
 *
 * Requirements:
 *   - One queue shared between producers and consumer.
 *   - Each reading carries a channel identifier so the consumer can route it
 *     to the correct filter state.
 *   - Consumer applies a 4-point moving average per channel.
 *   - If the queue is full the producer discards the sample and increments an
 *     overflow counter (non-blocking send).
 *   - All configuration constants are in one place for easy tuning.
 *
 * Target: FreeRTOS v10+ on ARM Cortex-M (simulated via POSIX port or
 *         FreeRTOS Windows Simulator for host-side testing).
 *
 * Compile (POSIX port example):
 *   gcc -std=c11 -Wall -Wextra -I<FreeRTOS_POSIX>/include \
 *       challenge_01_producer_consumer.c \
 *       <FreeRTOS sources> -lpthread -o producer_consumer
 *
 * Expected output (approximate — values are simulated):
 *   [CH0] raw=512 filtered=512
 *   [CH1] raw=256 filtered=256
 *   [CH0] raw=520 filtered=514
 *   ...
 *   [STATS] ch0_overflows=0  ch1_overflows=0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>   /* rand() for simulated ADC noise */
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* -------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------*/

#define QUEUE_DEPTH             20U     /* max items in the shared queue     */
#define FILTER_WINDOW           4U      /* moving-average window size        */
#define SAMPLE_PERIOD_MS        10U     /* producer sampling interval        */
#define CONSUMER_TIMEOUT_MS     50U     /* consumer max wait for an item     */
#define STATS_PERIOD_MS         5000U   /* how often to print overflow stats */
#define NUM_CHANNELS            2U      /* number of ADC channels / producers */

/* Task stack depths (in words) and priorities */
#define PRODUCER_STACK_WORDS    256U
#define CONSUMER_STACK_WORDS    512U
#define STATS_STACK_WORDS       256U

#define PRIORITY_PRODUCER       2U
#define PRIORITY_CONSUMER       3U      /* consumer is higher: drains promptly */
#define PRIORITY_STATS          1U

/* -------------------------------------------------------------------------
 * Data types
 * -------------------------------------------------------------------------*/

typedef struct {
    uint8_t  ucChannel;     /* which ADC channel produced this sample        */
    uint16_t usRawValue;    /* 12-bit ADC reading (0-4095)                   */
    TickType_t xTimestamp;  /* tick count at time of sampling                */
} AdcSample_t;

/* Per-channel state held by the consumer */
typedef struct {
    uint16_t ausWindow[FILTER_WINDOW];   /* circular buffer of last N samples */
    uint8_t  ucWriteIndex;               /* next write position               */
    uint8_t  ucCount;                    /* samples accumulated (< FILTER_WINDOW at start) */
    uint32_t uOverflows;                 /* queue-full discards by producer   */
} ChannelState_t;

/* Parameters passed to each producer task at creation */
typedef struct {
    uint8_t      ucChannel;             /* ADC channel this producer owns    */
    uint16_t     usBaseValue;           /* simulated ADC baseline            */
    QueueHandle_t xQueue;              /* the shared sample queue           */
    uint32_t     *puOverflowCounter;    /* incremented on queue-full discard */
} ProducerParams_t;

/* -------------------------------------------------------------------------
 * Simulated ADC
 *
 * In a real system this would read a hardware ADC peripheral.
 * Here we return a base value plus a small pseudo-random noise term so the
 * moving-average has something meaningful to do.
 * -------------------------------------------------------------------------*/
static uint16_t adc_read(uint8_t ucChannel, uint16_t usBase)
{
    (void)ucChannel;
    /* +/- 8 LSB noise around the baseline */
    int noise = (rand() % 17) - 8;
    int raw   = (int)usBase + noise;
    /* Clamp to 12-bit ADC range */
    if (raw < 0)    raw = 0;
    if (raw > 4095) raw = 4095;
    return (uint16_t)raw;
}

/* -------------------------------------------------------------------------
 * Moving-average filter
 *
 * Computes the integer average of the last FILTER_WINDOW samples.
 * Returns the input value unchanged until the window is full.
 * -------------------------------------------------------------------------*/
static uint16_t filter_update(ChannelState_t *pxState, uint16_t usNewSample)
{
    /* Write the new sample into the circular window */
    pxState->ausWindow[pxState->ucWriteIndex] = usNewSample;
    pxState->ucWriteIndex = (pxState->ucWriteIndex + 1U) % FILTER_WINDOW;

    if (pxState->ucCount < FILTER_WINDOW) {
        pxState->ucCount++;
    }

    /* Average the valid entries */
    uint32_t uSum = 0U;
    for (uint8_t i = 0U; i < pxState->ucCount; i++) {
        uSum += pxState->ausWindow[i];
    }
    return (uint16_t)(uSum / pxState->ucCount);
}

/* -------------------------------------------------------------------------
 * Producer task
 *
 * Samples one ADC channel at a fixed rate and posts readings to the shared
 * queue.  If the queue is full, the sample is discarded and the overflow
 * counter is incremented.  The task uses vTaskDelayUntil to maintain a
 * precise sampling period regardless of how long the ADC read takes.
 * -------------------------------------------------------------------------*/
static void producer_task(void *pvParameters)
{
    ProducerParams_t *pxParams = (ProducerParams_t *)pvParameters;
    TickType_t        xLastWakeTime = xTaskGetTickCount();

    for (;;) {
        /* Wait until the next sample time — precise period, no drift */
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));

        AdcSample_t xSample = {
            .ucChannel    = pxParams->ucChannel,
            .usRawValue   = adc_read(pxParams->ucChannel, pxParams->usBaseValue),
            .xTimestamp   = xTaskGetTickCount(),
        };

        /* Non-blocking send: do not block the producer on a full queue.
           The consumer should drain faster than we produce (higher priority),
           but budget for transient bursts.                                    */
        if (xQueueSend(pxParams->xQueue, &xSample, 0) != pdTRUE) {
            /* Queue was full: discard this sample, record the overflow */
            (*pxParams->puOverflowCounter)++;
        }
    }
}

/* -------------------------------------------------------------------------
 * Consumer task
 *
 * Receives samples from the shared queue and routes each to the correct
 * per-channel state.  Applies the moving-average filter and prints the
 * result.  Runs at higher priority than the producers to ensure the queue
 * is drained promptly.
 * -------------------------------------------------------------------------*/
static void consumer_task(void *pvParameters)
{
    QueueHandle_t  xQueue  = (QueueHandle_t)pvParameters;
    ChannelState_t axState[NUM_CHANNELS];
    AdcSample_t    xSample;

    /* Initialise per-channel state */
    for (uint8_t i = 0U; i < NUM_CHANNELS; i++) {
        memset(&axState[i], 0, sizeof(ChannelState_t));
    }

    for (;;) {
        /* Block until a sample arrives or the timeout expires */
        if (xQueueReceive(xQueue, &xSample, pdMS_TO_TICKS(CONSUMER_TIMEOUT_MS))
                == pdTRUE) {
            if (xSample.ucChannel < NUM_CHANNELS) {
                uint16_t usFiltered =
                    filter_update(&axState[xSample.ucChannel], xSample.usRawValue);

                printf("[CH%u] raw=%4u  filtered=%4u  (t=%lu)\n",
                       (unsigned)xSample.ucChannel,
                       (unsigned)xSample.usRawValue,
                       (unsigned)usFiltered,
                       (unsigned long)xSample.xTimestamp);
            } else {
                /* Defensive: log unexpected channel index */
                printf("[CONSUMER] WARNING: unexpected channel %u\n",
                       (unsigned)xSample.ucChannel);
            }
        }
        /* If receive timed out: nothing to do — loop and wait again */
    }
}

/* -------------------------------------------------------------------------
 * Statistics task
 *
 * Periodically prints overflow counters and queue occupancy.
 * Runs at the lowest application priority so it only runs when nothing
 * else needs the CPU.
 * -------------------------------------------------------------------------*/
typedef struct {
    QueueHandle_t  xQueue;
    uint32_t      *auOverflows;   /* array[NUM_CHANNELS] */
} StatsParams_t;

static void stats_task(void *pvParameters)
{
    StatsParams_t *pxStats = (StatsParams_t *)pvParameters;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATS_PERIOD_MS));

        UBaseType_t uxWaiting = uxQueueMessagesWaiting(pxStats->xQueue);
        UBaseType_t uxSpaces  = uxQueueSpacesAvailable(pxStats->xQueue);

        printf("[STATS] queue: %u waiting, %u free",
               (unsigned)uxWaiting, (unsigned)uxSpaces);

        for (uint8_t i = 0U; i < NUM_CHANNELS; i++) {
            printf("  ch%u_overflows=%lu",
                   (unsigned)i, (unsigned long)pxStats->auOverflows[i]);
        }
        printf("\n");
    }
}

/* -------------------------------------------------------------------------
 * Application entry point
 * -------------------------------------------------------------------------*/

/* Overflow counters: one per producer (accessed by both producer task and
   stats task — acceptable because overflow counter updates are single-word
   writes on 32-bit Cortex-M, and the stats task only reads them).           */
static uint32_t        auOverflows[NUM_CHANNELS];
static ProducerParams_t axProducerParams[NUM_CHANNELS];
static StatsParams_t   xStatsParams;

int main(void)
{
    /* ---- Create the shared queue ---- */
    QueueHandle_t xQueue = xQueueCreate(QUEUE_DEPTH, sizeof(AdcSample_t));
    configASSERT(xQueue != NULL);

    /* ---- Initialise producer parameters ---- */
    const uint16_t ausBaselines[NUM_CHANNELS] = { 2048U, 1024U };

    for (uint8_t i = 0U; i < NUM_CHANNELS; i++) {
        axProducerParams[i].ucChannel       = i;
        axProducerParams[i].usBaseValue     = ausBaselines[i];
        axProducerParams[i].xQueue          = xQueue;
        axProducerParams[i].puOverflowCounter = &auOverflows[i];
        auOverflows[i] = 0U;
    }

    /* ---- Create tasks ---- */

    /* Producer tasks: two instances, one per channel */
    for (uint8_t i = 0U; i < NUM_CHANNELS; i++) {
        char acName[8];
        snprintf(acName, sizeof(acName), "PROD%u", (unsigned)i);
        BaseType_t xResult = xTaskCreate(
            producer_task,
            acName,
            PRODUCER_STACK_WORDS,
            &axProducerParams[i],
            PRIORITY_PRODUCER,
            NULL);
        configASSERT(xResult == pdPASS);
    }

    /* Consumer task: single instance, higher priority than producers */
    BaseType_t xResult = xTaskCreate(
        consumer_task,
        "CONSUMER",
        CONSUMER_STACK_WORDS,
        xQueue,           /* pass queue handle as parameter */
        PRIORITY_CONSUMER,
        NULL);
    configASSERT(xResult == pdPASS);

    /* Statistics task: lowest priority */
    xStatsParams.xQueue      = xQueue;
    xStatsParams.auOverflows = auOverflows;
    xResult = xTaskCreate(
        stats_task,
        "STATS",
        STATS_STACK_WORDS,
        &xStatsParams,
        PRIORITY_STATS,
        NULL);
    configASSERT(xResult == pdPASS);

    /* ---- Start the scheduler — this call does not return ---- */
    vTaskStartScheduler();

    /* Should never reach here */
    for (;;) {}
}

/* -------------------------------------------------------------------------
 * FreeRTOS application hooks
 * -------------------------------------------------------------------------*/

void vApplicationMallocFailedHook(void)
{
    /* Called if pvPortMalloc fails.  Signal fatal error. */
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    /* Called if a task stack overflows.  Signal fatal error. */
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

/* =========================================================================
 * INTERVIEW DISCUSSION NOTES
 *
 * Q: Why is the consumer at higher priority than the producers?
 * A: The consumer must drain the queue faster than the producers fill it
 *    on average.  With PRIORITY_CONSUMER > PRIORITY_PRODUCER, whenever a
 *    producer puts an item in the queue, the consumer is preempted-in
 *    immediately (via portYIELD_FROM_ISR path inside xQueueSend) and
 *    processes the item before the next producer tick.  If the consumer
 *    were lower priority, the queue would fill up during any burst of
 *    producer activity, causing overflows.
 *
 * Q: Why does the producer use xQueueSend with timeout=0 (non-blocking)?
 * A: The producer has a strict periodic timing requirement (vTaskDelayUntil).
 *    If it blocked on a full queue, it would miss its next sampling deadline.
 *    The design choice is: prefer timing accuracy over data completeness.
 *    An alternative is to increase the queue depth to absorb bursts.
 *
 * Q: Are the overflow counters thread-safe?
 * A: The write is done by one producer task (single writer) and read by the
 *    stats task (single reader).  On 32-bit Cortex-M, a 32-bit word write/
 *    read is atomic.  However, the counter could be missed by stats_task
 *    between increments.  For precise counting, use an atomic increment or
 *    a mutex.  For a diagnostic counter, the current design is acceptable.
 *
 * Q: How would you extend this to use ISR-based ADC sampling?
 * A: Replace the vTaskDelayUntil loop with an ADC interrupt handler that
 *    calls xQueueSendFromISR.  The producer task is eliminated.  The consumer
 *    task remains unchanged.  Remember to call portYIELD_FROM_ISR to trigger
 *    an immediate context switch to the consumer if it has higher priority.
 * =========================================================================*/

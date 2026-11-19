#ifndef EVENTS_H
#define EVENTS_H

#include <stdint.h>

// События модели. Значения должны совпадать с EventKind из protocol.h,
// чтобы сервер мог передавать их наблюдателям без потери смысла.
typedef enum {
    MODEL_EVENT_CAR_ARRIVED = 1,
    MODEL_EVENT_CAR_QUEUED,
    MODEL_EVENT_CAR_REJECTED,
    MODEL_EVENT_CAR_PASSED,
    MODEL_EVENT_PED_REQUEST,
    MODEL_EVENT_PED_QUEUED,
    MODEL_EVENT_PED_REJECTED,
    MODEL_EVENT_PED_PASSED,
    MODEL_EVENT_PHASE_START,
    MODEL_EVENT_PHASE_END,
    MODEL_EVENT_TRANSITION_START,
    MODEL_EVENT_TRANSITION_END,
    MODEL_EVENT_EMERGENCY_PRIORITY,
    MODEL_EVENT_SERVER_START,
    MODEL_EVENT_SERVER_STOP
} ModelEventKind;

typedef struct {
    unsigned long tick;
    uint32_t participant_id;
    ModelEventKind kind;
    int direction;
    int maneuver;
    int phase;
    int state;
    int queue_cars;
    int queue_peds;
    int emergency;
} ModelEvent;

typedef void (*ModelEventSink)(const ModelEvent *ev, void *ctx);

#endif /* EVENTS_H */

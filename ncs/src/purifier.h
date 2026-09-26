#ifndef PURIFIER_H
#define PURIFIER_H
#include "aoia_protocol.h"

struct purifier_snapshot {
    struct aoia_state state;
    bool known;
    bool pending;
    uint32_t revision;
    uint32_t completed; /* Forces a report even when a command was refused by hardware. */
};
int purifier_init(void);
void purifier_process(void); /* Call only from the hardware/main thread. */
void purifier_snapshot(struct purifier_snapshot *out);
/* Nonblocking, rejects unknown/stale state or an outstanding command. */
int purifier_request(enum aoia_control control, uint8_t value);
#endif

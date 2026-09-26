#ifndef AOIA_FLOAT_REPORTING_H
#define AOIA_FLOAT_REPORTING_H
#include <stdbool.h>
#include <math.h>
#include <string.h>

/* IEEE-754 float ZCL reporting, including transition to/from unknown. Use
 * memcpy: Nordic reporting unions and packed attributes need not be aligned. */
static inline bool aoia_float_report_due(const void *current, const void *previous,
                                         const void *change)
{
    float now, before, delta;
    memcpy(&now, current, sizeof(now));
    memcpy(&before, previous, sizeof(before));
    memcpy(&delta, change, sizeof(delta));
    if (isnan(now) || isnan(before)) { return isnan(now) != isnan(before); }
    return fabsf(now - before) >= delta;
}
#endif

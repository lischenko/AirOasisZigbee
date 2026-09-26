#include "aoia_protocol.h"
#include <string.h>

int aoia_decode_mode(uint8_t speed)
{
    switch (speed) {
    case 0x11: return AOIA_AUTO;
    case 0x13: return AOIA_LOW;
    case 0x23: return AOIA_MEDIUM;
    case 0x33: return AOIA_HIGH;
    case 0x43: return AOIA_MAX;
    default: return -1;
    }
}

bool aoia_parse_byte(struct aoia_parser *p, uint8_t byte, struct aoia_state *state)
{
    if (p->used == 0 && byte != 0xac) { return false; }
    p->data[p->used++] = byte;
    if (p->used != AOIA_RX_SIZE) { return false; }
    uint8_t sum = 0;
    for (size_t i = 0; i < 15; ++i) { sum += p->data[i]; }
    if (byte == 0x44 && sum == 0xac) {
        uint16_t counter = ((uint16_t)p->data[9] << 8) | p->data[10];
        unsigned percent = ((unsigned)counter * 100u + 2200u) / 4400u;
        *state = (struct aoia_state) {
            .flags = p->data[1], .speed = p->data[2], .pm25 = p->data[5],
            .filter_percent = percent > 100u ? 100u : percent,
        };
        p->used = 0;
        return true;
    }
    /* Retain a possible embedded start after noise/dropped bytes. Unlike the
     * PoC's fixed discard this cannot lose the following complete frame. */
    size_t next = 1;
    while (next < AOIA_RX_SIZE && p->data[next] != 0xac) { ++next; }
    p->used = AOIA_RX_SIZE - next;
    memmove(p->data, p->data + next, p->used);
    return false;
}

uint8_t aoia_fan_mode(const struct aoia_state *s)
{
    if (!(s->flags & AOIA_POWER)) { return 0; }
    switch (aoia_decode_mode(s->speed)) {
    case AOIA_AUTO: return 5;
    case AOIA_LOW: return 1;
    case AOIA_MEDIUM: return 2;
    case AOIA_HIGH:
    case AOIA_MAX: return 3;
    default: return 0xff; /* ZCL invalid enum, not a fabricated speed. */
    }
}

bool aoia_boost(const struct aoia_state *s)
{
    return (s->flags & AOIA_POWER) && s->speed == 0x43;
}

bool aoia_make_command(const struct aoia_state *s, enum aoia_control control,
                       uint8_t value, uint8_t out[AOIA_TX_SIZE])
{
    uint8_t flags = s->flags, speed = s->speed, mask = 0;
    switch (control) {
    case AOIA_CTL_FAN:
        switch (value) {
        case 0: flags &= ~AOIA_POWER; break;
        case 1: speed = 0x13; flags |= AOIA_POWER; break;
        case 2: speed = 0x23; flags |= AOIA_POWER; break;
        case 3: speed = 0x33; flags |= AOIA_POWER; break;
        /* FanModeSequence=Low/Medium/High/Auto: standard On is not a
         * selectable mode here. This avoids advertising a non-sequence mode
         * or falsely calling it the purifier's fourth (Max) speed. */
        case 5: speed = 0x41; flags |= AOIA_POWER; break; /* Auto TX != RX. */
        default: return false; /* No occupancy information: Smart unsupported. */
        }
        break;
    case AOIA_CTL_POWER: mask = AOIA_POWER; break;
    case AOIA_CTL_IONIZER: mask = AOIA_IONIZER; break;
    case AOIA_CTL_UV: mask = AOIA_UV; break;
    case AOIA_CTL_NIGHT: mask = AOIA_NIGHT; break;
    case AOIA_CTL_BOOST:
        if (value > 1) { return false; }
        if (value) { speed = 0x43; flags |= AOIA_POWER; }
        else if (s->speed == 0x43) { speed = 0x33; }
        break;
    default: return false;
    }
    if (mask) {
        if (value > 1) { return false; }
        flags = value ? flags | mask : flags & ~mask;
    }
    /* Preserve every unrelated/reserved flag and the received speed for option
     * commands, exactly as the working PoC does. No speculative retries. */
    const uint8_t frame[AOIA_TX_SIZE] = {0xbb, 0x02, flags, speed, 0x03,
                                         0, 0, 0, 0, 0, 0, 0x44};
    memcpy(out, frame, AOIA_TX_SIZE);
    uint8_t sum = 0;
    for (size_t i = 0; i < 10; ++i) { sum += out[i]; }
    out[10] = 0xbb - sum;
    return true;
}

/* Host-only tests: no Zephyr or Zigbee dependency. */
#include "aoia_protocol.h"
#include <assert.h>
#include <string.h>

static void state_frame(uint8_t frame[16], uint8_t flags, uint8_t speed,
                        uint8_t pm25, uint16_t filter)
{
    memset(frame, 0, 16);
    frame[0] = 0xac; frame[1] = flags; frame[2] = speed; frame[5] = pm25;
    frame[9] = filter >> 8; frame[10] = filter; frame[15] = 0x44;
    uint8_t sum = 0;
    for (int i = 0; i != 15; ++i) { sum += frame[i]; }
    /* The controller's checksum convention says sum(first 15) == 0xac. */
    frame[14] = 0xac - sum;
}

static struct aoia_state parse(const uint8_t frame[16])
{
    struct aoia_parser parser = {0};
    struct aoia_state state = {0};
    for (int i = 0; i != 16; ++i) {
        assert(aoia_parse_byte(&parser, frame[i], &state) == (i == 15));
    }
    return state;
}

static void test_parse_and_resync(void)
{
    uint8_t frame[16];
    state_frame(frame, AOIA_POWER | AOIA_UV, 0x13, 17, 1408);
    struct aoia_state s = parse(frame);
    assert(s.flags == (AOIA_POWER | AOIA_UV));
    assert(s.speed == 0x13 && s.pm25 == 17 && s.filter_percent == 32);

    /* A corrupted frame followed by a valid start cannot poison the next one. */
    struct aoia_parser parser = {0};
    frame[14]++;
    for (int i = 0; i != 16; ++i) { assert(!aoia_parse_byte(&parser, frame[i], &s)); }
    state_frame(frame, AOIA_POWER, 0x11, 0, 4400);
    for (int i = 0; i != 16; ++i) {
        assert(aoia_parse_byte(&parser, frame[i], &s) == (i == 15));
    }
    assert(s.speed == 0x11 && s.filter_percent == 100);
}

static void check_checksum(const uint8_t command[12])
{
    uint8_t sum = 0;
    for (int i = 0; i != 11; ++i) { sum += command[i]; }
    assert(sum == 0xbb && command[0] == 0xbb && command[1] == 2 && command[11] == 0x44);
}

static void test_mode_and_controls(void)
{
    struct aoia_state s = {.flags = AOIA_POWER | AOIA_UV, .speed = 0x13};
    uint8_t out[12];
    assert(aoia_fan_mode(&s) == 1);
    assert(aoia_make_command(&s, AOIA_CTL_FAN, 1, out) && out[3] == 0x13);
    assert(aoia_make_command(&s, AOIA_CTL_FAN, 2, out) && out[3] == 0x23);
    assert(aoia_make_command(&s, AOIA_CTL_FAN, 0, out) && !(out[2] & AOIA_POWER));
    assert(aoia_make_command(&s, AOIA_CTL_FAN, 5, out));
    assert(out[2] == s.flags && out[3] == 0x41); /* TX Auto encoding */
    check_checksum(out);
    assert(aoia_make_command(&s, AOIA_CTL_FAN, 3, out));
    assert(out[3] == 0x33 && (out[2] & AOIA_POWER));
    assert(!aoia_make_command(&s, AOIA_CTL_FAN, 4, out)); /* On isn't Max */
    assert(aoia_make_command(&s, AOIA_CTL_BOOST, 1, out));
    assert(out[3] == 0x43 && aoia_boost(&(struct aoia_state){.flags=AOIA_POWER,.speed=0x43}));
    assert(aoia_make_command(&s, AOIA_CTL_IONIZER, 1, out));
    assert((out[2] & AOIA_IONIZER) && (out[2] & AOIA_UV));
    assert(aoia_make_command(&s, AOIA_CTL_POWER, 0, out));
    assert(!(out[2] & AOIA_POWER) && out[3] == 0x13);
}

int main(void)
{
    test_parse_and_resync();
    test_mode_and_controls();
    return 0;
}

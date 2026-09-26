#ifndef AOIA_PROTOCOL_H
#define AOIA_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AOIA_POWER 0x01u
#define AOIA_NIGHT 0x04u
#define AOIA_UV 0x08u
#define AOIA_IONIZER 0x20u
#define AOIA_RX_SIZE 16u
#define AOIA_TX_SIZE 12u

enum aoia_mode { AOIA_AUTO, AOIA_LOW, AOIA_MEDIUM, AOIA_HIGH, AOIA_MAX };
enum aoia_control { AOIA_CTL_FAN, AOIA_CTL_POWER, AOIA_CTL_IONIZER,
                    AOIA_CTL_UV, AOIA_CTL_NIGHT, AOIA_CTL_BOOST };

struct aoia_state {
    uint8_t flags;
    uint8_t speed; /* Actual received code, including Auto's asymmetric 0x11. */
    uint8_t pm25;
    uint8_t filter_percent;
};

struct aoia_parser { uint8_t data[AOIA_RX_SIZE]; size_t used; };

bool aoia_parse_byte(struct aoia_parser *parser, uint8_t byte, struct aoia_state *state);
int aoia_decode_mode(uint8_t speed);
uint8_t aoia_fan_mode(const struct aoia_state *state);
bool aoia_boost(const struct aoia_state *state);
bool aoia_make_command(const struct aoia_state *state, enum aoia_control control,
                       uint8_t value, uint8_t frame[AOIA_TX_SIZE]);
#endif

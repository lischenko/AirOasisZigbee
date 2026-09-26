#include "purifier.h"
#include "zigbee_device.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    struct purifier_snapshot s;
    purifier_snapshot(&s);
    shell_print(sh, "known=%d pending=%d flags=%02x speed=%02x PM2.5=%u filter=%u%%",
                s.known, s.pending, s.state.flags, s.state.speed, s.state.pm25,
                s.state.filter_percent);
    return 0;
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    if (strcmp(argv[1], "confirm")) {
        shell_error(sh, "Use: aoia factory_reset confirm (erases Zigbee pairing only)");
        return -EINVAL;
    }
    shell_print(sh, "Resetting Zigbee network; appliance settings untouched");
    zigbee_device_factory_reset();
    return 0;
}
SHELL_STATIC_SUBCMD_SET_CREATE(aoia_commands,
    SHELL_CMD_ARG(status, NULL, "Show confirmed UART state", cmd_status, 1, 0),
    SHELL_CMD_ARG(factory_reset, NULL, "factory_reset confirm: forget Zigbee network", cmd_reset, 2, 0),
    SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(aoia, &aoia_commands, "AirOasis diagnostics", NULL);

int main(void)
{
    /* Current Zephyr USBD CDC ACM initializes at boot; do not wait for USB/DTR. */
    int err = purifier_init();
    if (err) {
        LOG_ERR("Cannot initialize purifier UART: %d; not starting Zigbee", err);
        return err;
    }
    zigbee_device_init();
    for (;;) {
        purifier_process();
        k_sleep(K_MSEC(5));
    }
    return 0;
}

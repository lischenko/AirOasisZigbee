#include "zigbee_device.h"
#include "purifier.h"
#include <errno.h>
#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zboss_api.h>
#include <zb_nrf_platform.h>
#include <zigbee/zigbee_app_utils.h>
#include <zigbee/zigbee_error_handler.h>
#include <zcl/zb_zcl_fan_control.h>
#include <zcl/zb_zcl_pm2_5_measurement.h>
#include <zcl/zb_zcl_analog_input.h>

LOG_MODULE_REGISTER(aoia_zigbee, LOG_LEVEL_INF);
#define FAN ZB_ZCL_CLUSTER_ID_FAN_CONTROL
#define PM ZB_ZCL_CLUSTER_ID_PM2_5_MEASUREMENT
#define ANALOG ZB_ZCL_CLUSTER_ID_ANALOG_INPUT
#define BINARY_OUTPUT ZB_ZCL_CLUSTER_ID_BINARY_OUTPUT
#define BASIC ZB_ZCL_CLUSTER_ID_BASIC
#define IDENTIFY ZB_ZCL_CLUSTER_ID_IDENTIFY
#define RO ZB_ZCL_ATTR_ACCESS_READ_ONLY
#define RW ZB_ZCL_ATTR_ACCESS_READ_WRITE
#define REPORT ZB_ZCL_ATTR_ACCESS_REPORTING
#define ATTR(id, type, access, ptr) \
    {id, type, access, ZB_ZCL_NON_MANUFACTURER_SPECIFIC, (void *)(ptr)}
#define SERVER(id, attrs) {id, ZB_ZCL_ARRAY_SIZE(attrs, zb_zcl_attr_t), attrs, \
    ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ZCL_MANUF_CODE_INVALID, id##_SERVER_ROLE_INIT}
/* Binary Output is a standard ZCL cluster but has no prebuilt R23 source
 * handler. It has no cluster commands, so foundation commands are sufficient. */
#define SERVER_NO_INIT(id, attrs) {id, ZB_ZCL_ARRAY_SIZE(attrs, zb_zcl_attr_t), attrs, \
    ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ZCL_MANUF_CODE_INVALID, NULL}

static zb_uint8_t zcl_version = ZB_ZCL_VERSION, app_version = 1, hw_version = 1;
static zb_uint8_t power_source = ZB_ZCL_BASIC_POWER_SOURCE_MAINS_SINGLE_PHASE;
static zb_char_t manufacturer[] = "\x08" "AirOasis";
static zb_char_t model[] = "\x0e" "AOIA-NCS-ZB-v1";
static zb_char_t build_id[] = "\x05" "1.0.0";
static zb_char_t labels[6][32] = {
    "\x08" "Purifier", "\x07" "Ionizer", "\x02" "UV",
    "\x12" "Night / child lock", "\x09" "Max boost", "\x05" "Power"
};
static zb_uint16_t revision = 1, basic_revision = 3, pm_revision = 2;
static zb_uint16_t identify_time[6];
/* Bool/enum attributes always remain valid ZCL values. Before the first
 * response they conservatively read Off; stale control state remains last
 * confirmed rather than being changed to an invalid sentinel. */
static zb_uint8_t fan_mode, fan_sequence = 2;
static zb_uint8_t binary[5];
static float pm25 = NAN, pm_min = 0, pm_max = 255;
static float filter = NAN, filter_min = 0, filter_max = 100, resolution = 1;
static zb_char_t filter_description[] = "\x0b" "Filter life";
static zb_uint16_t percent_units = 98; /* BACnet/ZCL engineering units: percent. */
static zb_uint8_t out_of_service, status_flags = 2, reliability = 7;

/* ProductLabel (ZCL8 Basic 0x000e) describes an endpoint; it is not an HA UI name.
 * No guessed LED GPIO: Identify is observable through the USB diagnostic log. */
#define DECLARE_COMMON(n) \
    static zb_zcl_attr_t basic_##n[] = { \
        ATTR(0x0000, ZB_ZCL_ATTR_TYPE_U8, RO, &zcl_version), \
        ATTR(0x0001, ZB_ZCL_ATTR_TYPE_U8, RO, &app_version), \
        ATTR(0x0003, ZB_ZCL_ATTR_TYPE_U8, RO, &hw_version), \
        ATTR(0x0004, ZB_ZCL_ATTR_TYPE_CHAR_STRING, RO, manufacturer), \
        ATTR(0x0005, ZB_ZCL_ATTR_TYPE_CHAR_STRING, RO, model), \
        ATTR(0x0007, ZB_ZCL_ATTR_TYPE_8BIT_ENUM, RO, &power_source), \
        ATTR(0x000e, ZB_ZCL_ATTR_TYPE_CHAR_STRING, RO, labels[n-1]), \
        ATTR(0x4000, ZB_ZCL_ATTR_TYPE_CHAR_STRING, RO, build_id), \
        ATTR(0xfffd, ZB_ZCL_ATTR_TYPE_U16, RO, &basic_revision), \
    }; \
    ZB_ZCL_DECLARE_IDENTIFY_ATTRIB_LIST(identify_##n, &identify_time[n-1])
DECLARE_COMMON(1);
DECLARE_COMMON(2);
DECLARE_COMMON(3);
DECLARE_COMMON(4);
DECLARE_COMMON(5);
DECLARE_COMMON(6);

/* Nordic's Fan macro lacks REPORTING: ZCL permits optional reporting, ZHA binds it. */
static zb_zcl_attr_t fan_attrs[] = {
    ATTR(0, ZB_ZCL_ATTR_TYPE_8BIT_ENUM, RW | REPORT, &fan_mode),
    ATTR(1, ZB_ZCL_ATTR_TYPE_8BIT_ENUM, RW, &fan_sequence),
    ATTR(0xfffd, ZB_ZCL_ATTR_TYPE_U16, RO, &revision),
};
/* Deliberately omit optional Tolerance: the PoC has no sensor accuracy data.
 * Nordic's tolerance macro marks it manufacturer-specific; no such extension here. */
static zb_zcl_attr_t pm_attrs[] = {
    ATTR(0, ZB_ZCL_ATTR_TYPE_SINGLE, RO | REPORT, &pm25),
    ATTR(1, ZB_ZCL_ATTR_TYPE_SINGLE, RO, &pm_min),
    ATTR(2, ZB_ZCL_ATTR_TYPE_SINGLE, RO, &pm_max),
    ATTR(0xfffd, ZB_ZCL_ATTR_TYPE_U16, RO, &pm_revision),
};
static zb_zcl_attr_t filter_attrs[] = {
    ATTR(0x001c, ZB_ZCL_ATTR_TYPE_CHAR_STRING, RO, filter_description),
    ATTR(0x0041, ZB_ZCL_ATTR_TYPE_SINGLE, RO, &filter_max),
    ATTR(0x0045, ZB_ZCL_ATTR_TYPE_SINGLE, RO, &filter_min),
    ATTR(0x0051, ZB_ZCL_ATTR_TYPE_BOOL, RO, &out_of_service),
    /* PresentValue is writable only out of service (ZCL 3.14.11.2). This
     * physical input keeps optional OutOfService writes disabled. */
    ATTR(0x0055, ZB_ZCL_ATTR_TYPE_SINGLE, RW | REPORT, &filter),
    ATTR(0x0067, ZB_ZCL_ATTR_TYPE_8BIT_ENUM, RO, &reliability),
    ATTR(0x006a, ZB_ZCL_ATTR_TYPE_SINGLE, RO, &resolution),
    ATTR(0x006f, ZB_ZCL_ATTR_TYPE_8BITMAP, RO | REPORT, &status_flags),
    ATTR(0x0075, ZB_ZCL_ATTR_TYPE_16BIT_ENUM, RO, &percent_units),
    ATTR(0xfffd, ZB_ZCL_ATTR_TYPE_U16, RO, &revision),
};
static zb_zcl_cluster_desc_t clusters_1[] = {
    SERVER(ZB_ZCL_CLUSTER_ID_BASIC, basic_1),
    SERVER(ZB_ZCL_CLUSTER_ID_IDENTIFY, identify_1),
    SERVER(ZB_ZCL_CLUSTER_ID_FAN_CONTROL, fan_attrs),
    SERVER(ZB_ZCL_CLUSTER_ID_PM2_5_MEASUREMENT, pm_attrs),
    SERVER(ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, filter_attrs),
};
ZB_DECLARE_SIMPLE_DESC(5, 0);
static ZB_AF_SIMPLE_DESC_TYPE(5, 0) desc_1 = {
    1, ZB_AF_HA_PROFILE_ID, 0x0007, 1, 0, 5, 0, {BASIC, IDENTIFY, FAN, PM, ANALOG}
};
ZBOSS_DEVICE_DECLARE_REPORTING_CTX(report_1, 4);
ZB_AF_DECLARE_ENDPOINT_DESC(ep_1, 1, ZB_AF_HA_PROFILE_ID, 0, NULL, 5, clusters_1,
    (zb_af_simple_desc_1_1_t *)&desc_1, 4, report_1, 0, NULL);

ZB_DECLARE_SIMPLE_DESC(3, 0);
/* Binary Output's standard Description is deliberately used here. ZHA's
 * BinaryOutputSwitch reads it as the entity fallback name; On/Off cannot. */
#define DECLARE_BINARY(n) \
    static zb_uint8_t binary_out_of_service_##n, binary_status_flags_##n; \
    static zb_zcl_attr_t binary_output_##n[] = { \
        ATTR(0x001c, ZB_ZCL_ATTR_TYPE_CHAR_STRING, RO, labels[n-1]), \
        ATTR(0x0051, ZB_ZCL_ATTR_TYPE_BOOL, RO, &binary_out_of_service_##n), \
        ATTR(0x0055, ZB_ZCL_ATTR_TYPE_BOOL, RW | REPORT, &binary[n-2]), \
        ATTR(0x006f, ZB_ZCL_ATTR_TYPE_8BITMAP, RO | REPORT, &binary_status_flags_##n), \
        ATTR(0xfffd, ZB_ZCL_ATTR_TYPE_U16, RO, &revision), \
    }; \
    static zb_zcl_cluster_desc_t clusters_##n[] = { \
        SERVER(ZB_ZCL_CLUSTER_ID_BASIC, basic_##n), \
        SERVER(ZB_ZCL_CLUSTER_ID_IDENTIFY, identify_##n), \
        SERVER_NO_INIT(ZB_ZCL_CLUSTER_ID_BINARY_OUTPUT, binary_output_##n), \
    }; \
    static ZB_AF_SIMPLE_DESC_TYPE(3, 0) desc_##n = { \
        n, ZB_AF_HA_PROFILE_ID, 0x0002, 1, 0, 3, 0, {BASIC, IDENTIFY, BINARY_OUTPUT} \
    }; \
    ZBOSS_DEVICE_DECLARE_REPORTING_CTX(report_##n, 1); \
    ZB_AF_DECLARE_ENDPOINT_DESC(ep_##n, n, ZB_AF_HA_PROFILE_ID, 0, NULL, 3, clusters_##n, \
        (zb_af_simple_desc_1_1_t *)&desc_##n, 1, report_##n, 0, NULL)
DECLARE_BINARY(2);
DECLARE_BINARY(3);
DECLARE_BINARY(4);
DECLARE_BINARY(5);
DECLARE_BINARY(6);
ZB_AF_START_DECLARE_ENDPOINT_LIST(endpoints)
    &ep_1, &ep_2, &ep_3, &ep_4, &ep_5, &ep_6,
ZB_AF_FINISH_DECLARE_ENDPOINT_LIST;
ZBOSS_DECLARE_DEVICE_CTX(device_ctx, endpoints, 6);

static const enum aoia_control binary_control[] = {
    AOIA_CTL_IONIZER, AOIA_CTL_UV, AOIA_CTL_NIGHT, AOIA_CTL_BOOST, AOIA_CTL_POWER
};

static uint8_t request_status(int err)
{
    if (!err) { return ZB_ZCL_STATUS_SUCCESS; }
    if (err == -EINVAL) { return ZB_ZCL_STATUS_INVALID_VALUE; }
    /* ZCL has no generic BUSY status. FAIL means the command was not accepted. */
    LOG_WRN("Control rejected: %d (unknown/stale state or command pending)", err);
    return ZB_ZCL_STATUS_FAIL;
}

static void finish(zb_bufid_t buf, zb_zcl_parsed_hdr_t *hdr, uint8_t status)
{
    /* Nordic handles disabled default responses, group/broadcast addressing,
     * buffer ownership and request direction here. */
    if (hdr->is_common_command && hdr->cmd_id == ZB_ZCL_CMD_WRITE_ATTRIB_NO_RESP) {
        zb_buf_free(buf);
    } else {
        zb_zcl_send_default_resp_ext(buf, hdr, status);
    }
}

/* Handle fan writes without modifying confirmed attributes speculatively. The
 * stock Fan cluster only validates enum ranges and has no hardware callback.
 * Foundation Read/Discover/Configure Reporting remain handled by ZBOSS. */
static void fan_write(zb_bufid_t buf, zb_zcl_parsed_hdr_t *hdr)
{
    struct record { uint16_t id; uint8_t status, value; } records[64];
    unsigned count = 0;
    uint8_t *data = zb_buf_begin(buf);
    zb_uint16_t len = zb_buf_len(buf);
    bool invalid = false;
    if (!len) { finish(buf, hdr, ZB_ZCL_STATUS_MALFORMED_CMD); return; }
    while (len && count < ARRAY_SIZE(records)) {
        zb_zcl_write_attr_req_t *req;
        ZB_ZCL_GENERAL_GET_NEXT_WRITE_ATTR_REQ(data, len, req);
        if (!req) { finish(buf, hdr, ZB_ZCL_STATUS_MALFORMED_CMD); return; }
        struct record *r = &records[count++];
        r->id = req->attr_id;
        r->value = req->attr_value[0];
        r->status = ZB_ZCL_STATUS_SUCCESS;
        if (r->id > 1) {
            r->status = r->id == 0xfffd ? ZB_ZCL_STATUS_READ_ONLY : ZB_ZCL_STATUS_UNSUP_ATTRIB;
        } else if (req->attr_type != ZB_ZCL_ATTR_TYPE_8BIT_ENUM) {
            r->status = ZB_ZCL_STATUS_INVALID_TYPE;
        } else if ((r->id == 0 && (r->value == 4 || r->value > 5)) ||
                   (r->id == 1 && r->value != 2)) {
            r->status = ZB_ZCL_STATUS_INVALID_VALUE;
        }
        invalid |= r->status != ZB_ZCL_STATUS_SUCCESS;
    }
    if (len) { finish(buf, hdr, ZB_ZCL_STATUS_INSUFF_SPACE); return; }
    /* An undivided write is validated in full before any appliance command.
     * Sequence can only remain 2. For repeated FanMode records the last wins. */
    int mode_record = -1;
    for (unsigned i = 0; i < count; ++i) {
        if (records[i].id == 0 && !records[i].status) { mode_record = i; }
    }
    if (mode_record >= 0 && !(invalid && hdr->cmd_id == ZB_ZCL_CMD_WRITE_ATTRIB_UNDIV)) {
        uint8_t status = request_status(purifier_request(AOIA_CTL_FAN, records[mode_record].value));
        if (status) {
            for (unsigned i = 0; i < count; ++i) {
                if (records[i].id == 0 && !records[i].status) { records[i].status = status; }
            }
        }
    }
    if (hdr->cmd_id == ZB_ZCL_CMD_WRITE_ATTRIB_NO_RESP) { zb_buf_free(buf); return; }
    zb_uint8_t *cmd_ptr;
    ZB_ZCL_GENERAL_INIT_WRITE_ATTR_RESP(buf, cmd_ptr, hdr->seq_number);
    bool failed = false;
    for (unsigned i = 0; i < count; ++i) {
        if (!records[i].status) { continue; }
        failed = true;
        ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, records[i].status);
        ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, records[i].id);
    }
    if (!failed) { ZB_ZCL_GENERAL_SUCCESS_WRITE_ATTR_RESP(cmd_ptr); }
    ZB_ZCL_GENERAL_SEND_WRITE_ATTR_RESP(buf, cmd_ptr,
        ZB_ZCL_PARSED_HDR_SHORT_DATA(hdr).source.u.short_addr,
        ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        ZB_ZCL_PARSED_HDR_SHORT_DATA(hdr).src_endpoint, 1, ZB_AF_HA_PROFILE_ID, FAN, NULL);
}

/* ZHA's standard BinaryOutputSwitch writes PresentValue rather than sending
 * On/Off commands. As with FanMode, defer the attribute change until AOIA
 * confirms it in a checksum-valid state frame. */
static void binary_output_write(zb_bufid_t buf, zb_zcl_parsed_hdr_t *hdr, uint8_t ep)
{
    struct record { uint16_t id; uint8_t status, value; } records[64];
    unsigned count = 0;
    uint8_t *data = zb_buf_begin(buf);
    zb_uint16_t len = zb_buf_len(buf);
    bool invalid = false;
    if (!len) { finish(buf, hdr, ZB_ZCL_STATUS_MALFORMED_CMD); return; }
    while (len && count < ARRAY_SIZE(records)) {
        zb_zcl_write_attr_req_t *req;
        ZB_ZCL_GENERAL_GET_NEXT_WRITE_ATTR_REQ(data, len, req);
        if (!req) { finish(buf, hdr, ZB_ZCL_STATUS_MALFORMED_CMD); return; }
        struct record *r = &records[count++];
        r->id = req->attr_id;
        r->value = req->attr_value[0];
        r->status = ZB_ZCL_STATUS_SUCCESS;
        if (r->id != 0x0055) {
            r->status = (r->id == 0x001c || r->id == 0x0051 || r->id == 0x006f || r->id == 0xfffd)
                        ? ZB_ZCL_STATUS_READ_ONLY : ZB_ZCL_STATUS_UNSUP_ATTRIB;
        } else if (req->attr_type != ZB_ZCL_ATTR_TYPE_BOOL || r->value > 1) {
            r->status = ZB_ZCL_STATUS_INVALID_VALUE;
        }
        invalid |= r->status != ZB_ZCL_STATUS_SUCCESS;
    }
    if (len) { finish(buf, hdr, ZB_ZCL_STATUS_INSUFF_SPACE); return; }
    int value_record = -1;
    for (unsigned i = 0; i < count; ++i) {
        if (records[i].id == 0x0055 && !records[i].status) { value_record = i; }
    }
    if (value_record >= 0 && !(invalid && hdr->cmd_id == ZB_ZCL_CMD_WRITE_ATTRIB_UNDIV)) {
        uint8_t status = request_status(purifier_request(binary_control[ep - 2], records[value_record].value));
        if (status) {
            for (unsigned i = 0; i < count; ++i) {
                if (records[i].id == 0x0055 && !records[i].status) { records[i].status = status; }
            }
        }
    }
    if (hdr->cmd_id == ZB_ZCL_CMD_WRITE_ATTRIB_NO_RESP) { zb_buf_free(buf); return; }
    zb_uint8_t *cmd_ptr;
    ZB_ZCL_GENERAL_INIT_WRITE_ATTR_RESP(buf, cmd_ptr, hdr->seq_number);
    bool failed = false;
    for (unsigned i = 0; i < count; ++i) {
        if (!records[i].status) { continue; }
        failed = true;
        ZB_ZCL_PACKET_PUT_DATA8(cmd_ptr, records[i].status);
        ZB_ZCL_PACKET_PUT_DATA16_VAL(cmd_ptr, records[i].id);
    }
    if (!failed) { ZB_ZCL_GENERAL_SUCCESS_WRITE_ATTR_RESP(cmd_ptr); }
    ZB_ZCL_GENERAL_SEND_WRITE_ATTR_RESP(buf, cmd_ptr,
        ZB_ZCL_PARSED_HDR_SHORT_DATA(hdr).source.u.short_addr,
        ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        ZB_ZCL_PARSED_HDR_SHORT_DATA(hdr).src_endpoint, ep, ZB_AF_HA_PROFILE_ID,
        BINARY_OUTPUT, NULL);
}

static zb_uint8_t endpoint_handler(zb_bufid_t buf)
{
    zb_zcl_parsed_hdr_t hdr;
    ZB_ZCL_COPY_PARSED_HEADER(buf, &hdr);
    if (hdr.cmd_direction != ZB_ZCL_FRAME_DIRECTION_TO_SRV || hdr.is_manuf_specific) { return ZB_FALSE; }
    uint8_t ep = ZB_ZCL_PARSED_HDR_SHORT_DATA(&hdr).dst_endpoint;
    if (ep == 1 && hdr.cluster_id == FAN && hdr.is_common_command &&
        (hdr.cmd_id == ZB_ZCL_CMD_WRITE_ATTRIB || hdr.cmd_id == ZB_ZCL_CMD_WRITE_ATTRIB_UNDIV ||
         hdr.cmd_id == ZB_ZCL_CMD_WRITE_ATTRIB_NO_RESP)) {
        fan_write(buf, &hdr);
        return ZB_TRUE;
    }
    if (ep >= 2 && ep <= 6 && hdr.cluster_id == BINARY_OUTPUT && hdr.is_common_command &&
        (hdr.cmd_id == ZB_ZCL_CMD_WRITE_ATTRIB || hdr.cmd_id == ZB_ZCL_CMD_WRITE_ATTRIB_UNDIV ||
         hdr.cmd_id == ZB_ZCL_CMD_WRITE_ATTRIB_NO_RESP)) {
        binary_output_write(buf, &hdr, ep);
        return ZB_TRUE;
    }
    return ZB_FALSE;
}

static bool updating_telemetry;

static zb_ret_t check_value(zb_uint16_t cluster, zb_uint8_t role, zb_uint8_t ep,
                            zb_uint16_t attr, zb_uint16_t manuf, zb_uint8_t *value)
{
    ARG_UNUSED(ep); ARG_UNUSED(attr); ARG_UNUSED(manuf); ARG_UNUSED(value);
    /* SDK PM validation compares NaN with == and Fan rejects invalid 0xff.
     * Bypass only for our locally validated telemetry (not remote writes).
     * Read-only access checks still run before this application value check. */
    if (updating_telemetry) { return RET_OK; }
    if (role == ZB_ZCL_CLUSTER_SERVER_ROLE && cluster == FAN) { return RET_ERROR; }
    if (role == ZB_ZCL_CLUSTER_SERVER_ROLE && cluster == ANALOG && attr == 0x55) {
        return RET_BLOCKED; /* physical read-only input; OutOfService remains false */
    }
    return RET_IGNORE;
}

static void set_attr(uint8_t ep, uint16_t cluster, uint16_t attr, void *value)
{
    updating_telemetry = true;
    zb_zcl_status_t status = zb_zcl_set_attr_val(ep, cluster, ZB_ZCL_CLUSTER_SERVER_ROLE,
                                                attr, value, ZB_FALSE);
    updating_telemetry = false;
    if (status != ZB_ZCL_STATUS_SUCCESS) { LOG_ERR("Set %u/%04x/%04x: %u", ep, cluster, attr, status); }
    /* ZBOSS marks reportable attributes changed by this local setter. A
     * reply-confirmed state is never fabricated merely to satisfy a command. */
}

static void sync_state(zb_uint8_t unused)
{
    ARG_UNUSED(unused);
    static uint32_t last_revision = UINT32_MAX, last_completed;
    struct purifier_snapshot s;
    purifier_snapshot(&s);
    if (s.revision != last_revision || s.completed != last_completed) {
        float pm_value = s.known ? s.state.pm25 : NAN;
        float filter_value = s.known ? s.state.filter_percent : NAN;
        set_attr(1, PM, 0, &pm_value);
        set_attr(1, ANALOG, 0x55, &filter_value);
        reliability = s.known ? 0 : 7; /* no fault / unreliable other */
        set_attr(1, ANALOG, 0x67, &reliability);
        uint8_t flags = s.known ? 0 : 2; /* fault bit */
        set_attr(1, ANALOG, 0x6f, &flags);
        if (s.known) {
            uint8_t mode = aoia_fan_mode(&s.state);
            set_attr(1, FAN, 0, &mode);
            uint8_t values[] = {!!(s.state.flags & AOIA_IONIZER), !!(s.state.flags & AOIA_UV),
                                !!(s.state.flags & AOIA_NIGHT), aoia_boost(&s.state),
                                !!(s.state.flags & AOIA_POWER)};
            for (unsigned i = 0; i < ARRAY_SIZE(values); ++i) {
                set_attr(i+2, BINARY_OUTPUT, 0x0055, &values[i]);
            }
        }
        last_revision = s.revision;
        last_completed = s.completed;
    }
    ZB_ERROR_CHECK(ZB_SCHEDULE_APP_ALARM(sync_state, 0, ZB_MILLISECONDS_TO_BEACON_INTERVAL(200)));
}

static void configure_report(uint8_t ep, uint16_t cluster, uint16_t attr,
                             uint16_t min, uint16_t max, float delta)
{
    zb_zcl_reporting_info_t info = {0};
    info.direction = ZB_ZCL_CONFIGURE_REPORTING_SEND_REPORT;
    info.ep = ep;
    info.cluster_id = cluster;
    info.cluster_role = ZB_ZCL_CLUSTER_SERVER_ROLE;
    info.attr_id = attr;
    info.manuf_code = ZB_ZCL_NON_MANUFACTURER_SPECIFIC;
    info.dst.profile_id = ZB_AF_HA_PROFILE_ID;
    info.u.send_info.min_interval = info.u.send_info.def_min_interval = min;
    info.u.send_info.max_interval = info.u.send_info.def_max_interval = max;
    if (cluster == PM || cluster == ANALOG) { memcpy(&info.u.send_info.delta, &delta, sizeof(delta)); }
    /* Defaults only on first boot/join; ZHA may configure different intervals. */
    ZB_ERROR_CHECK(zb_zcl_put_reporting_info(&info, ZB_TRUE));
}

static void identify(zb_uint8_t active)
{
    LOG_INF("Identify %s: AirOasis AOIA-NCS-ZB-v1", active ? "started" : "stopped");
}

void zboss_signal_handler(zb_bufid_t buf)
{
    zb_zdo_app_signal_hdr_t *hdr;
    zb_zdo_app_signal_type_t signal = zb_get_app_signal(buf, &hdr);
    zb_ret_t status = ZB_GET_APP_SIGNAL_STATUS(buf);
    LOG_INF("Zigbee signal %u status %d", signal, status);
    ZB_ERROR_CHECK(zigbee_default_signal_handler(buf));
    /* Stack restores NVRAM during startup. Do not overwrite a returning
     * coordinator's reporting configuration, even after an ordinary reboot. */
    if (signal == ZB_BDB_SIGNAL_DEVICE_FIRST_START && status == RET_OK) {
        configure_report(1, FAN, 0, 1, 900, 0);
        configure_report(1, PM, 0, 30, 300, 1.0f);
        configure_report(1, ANALOG, 0x55, 30, 3600, 1.0f);
        configure_report(1, ANALOG, 0x6f, 1, 900, 0);
        for (unsigned ep = 2; ep <= 6; ++ep) { configure_report(ep, BINARY_OUTPUT, 0x0055, 1, 900, 0); }
    }
    if (buf) { zb_buf_free(buf); }
}

static void reset_network(zb_uint8_t unused)
{
    ARG_UNUSED(unused);
    zb_bdb_reset_via_local_action(0);
}

void zigbee_device_factory_reset(void)
{
    ZB_ERROR_CHECK(ZB_SCHEDULE_APP_CALLBACK(reset_network, 0));
}

void zigbee_device_init(void)
{
    ZB_AF_REGISTER_DEVICE_CTX(&device_ctx);
    ZB_ZCL_CHECK_ATTR_VALUE_MANUF_CB(check_value);
    for (unsigned ep = 1; ep <= 6; ++ep) {
        ZB_AF_SET_ENDPOINT_HANDLER(ep, endpoint_handler);
        ZB_AF_SET_IDENTIFY_NOTIFICATION_HANDLER(ep, identify);
    }
    zb_set_rx_on_when_idle(ZB_TRUE); /* Mains-powered end device, NOT router/sleepy. */
    zb_set_ed_timeout(ED_AGING_TIMEOUT_64MIN);
    ZB_ERROR_CHECK(ZB_SCHEDULE_APP_CALLBACK(sync_state, 0));
    zigbee_enable();
}

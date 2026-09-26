#!/usr/bin/env python3
"""Generate build-local fixes for ncs-zigbee v1.4.0's SINGLE reporting.

The licensed SDK files stay in the user's west workspace/build directory;
no SDK source is vendored or modified in place. Fail closed if anchors change.
"""
from pathlib import Path
import sys


def once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise RuntimeError(f"ZBOSS v1.4.0 source mismatch: {old!r}")
    return text.replace(old, new)


def generate(source: Path, output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    common = (source / "zcl/zcl_common.c").read_text()
    # SINGLE is an analog ZCL data type: Configure Reporting includes a 4-byte
    # reportable change, which the unmodified SDK otherwise parses as a record.
    start = common.index("zb_uint8_t zb_zcl_get_analog_attribute_size(")
    end = common.index("#define ZCL_DEVICE_CONTEXT_ITERATOR", start)
    section = common[start:end]
    section = once(section,
                   "    case ZB_ZCL_ATTR_TYPE_S32:\n      ret = sizeof(zb_uint32_t);",
                   "    case ZB_ZCL_ATTR_TYPE_S32:\n    case ZB_ZCL_ATTR_TYPE_SINGLE:\n      ret = sizeof(zb_uint32_t);")
    section = once(section,
                   "    case ZB_ZCL_ATTR_TYPE_UTC_TIME:\n      ret = ZB_TRUE;",
                   "    case ZB_ZCL_ATTR_TYPE_UTC_TIME:\n    case ZB_ZCL_ATTR_TYPE_SINGLE:\n      ret = ZB_TRUE;")
    common = common[:start] + section + common[end:]
    (output / "zcl_common.c").write_text(common)

    reporting = (source / "zcl/zcl_reporting.c").read_text()
    reporting = '#include "float_reporting.h"\n' + reporting
    start = reporting.index("void zb_zcl_save_reported_value(")
    end = reporting.index("static zb_bool_t check_delta_value(", start)
    section = reporting[start:end]
    section = once(section, "      default:\n        break;", """      case ZB_ZCL_ATTR_TYPE_SINGLE:
        ZB_MEMCPY(&rep_info->u.send_info.reported_value, attr_desc->data_p, sizeof(zb_single_t));
        break;
      default:
        break;""")
    reporting = reporting[:start] + section + reporting[end:]
    start = reporting.index("static zb_bool_t check_delta_value(")
    before, section = reporting[:start], reporting[start:]
    section = once(section, "          case ZB_ZCL_ATTR_TYPE_U8:", """          case ZB_ZCL_ATTR_TYPE_SINGLE:
            ret = aoia_float_report_due(attr_desc->data_p,
                      &rep_info->u.send_info.reported_value,
                      &rep_info->u.send_info.delta) ? RET_OK : RET_IGNORE;
            break;
          case ZB_ZCL_ATTR_TYPE_U8:""")
    (output / "zcl_reporting.c").write_text(before + section)


if __name__ == "__main__":
    generate(Path(sys.argv[1]), Path(sys.argv[2]))

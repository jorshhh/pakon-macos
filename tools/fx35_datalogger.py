#!/usr/bin/env python3
"""
fx35_datalogger.py — parse KK FX35-datalogger captures from a real F-135+.

The captures come from Kai Kaufman's open-source FX35 Windows driver with
read-only logging hooks added (see the owner's Pakon Software repo,
tools/FX35-datalogger/CAPTURE_FORMATS.md). Two file kinds:

  ioctl_log.bin  "PLOG" — every IOCTL through the driver, timestamped.
                 Crucially, IOCTL_PAKON_SEND_AND_RECEIVE_PACKET passes its
                 input buffer VERBATIM to the bulk command pipe
                 (Ezusb_Read_Write_Direct), so each input payload IS the raw
                 EP1 wire frame [type][count][addr][payload_len][cmd][data…] —
                 the same frame format as docs/PROTOCOL.md. No translation.
  ep6_NNN.bin    "EP6L" — the raw bulk image stream, one timestamped record
                 per USB packet.

Subcommands:
  dump    IOCTL_LOG            decoded human-readable timeline (PPB frames
                               annotated with address/command names)
  pakscan IOCTL_LOG OUT        emit a pakon_replay-compatible .pakscan script
                               (O lines from PPB frames, C lines from vendor
                               requests). Replies were not captured (driver
                               logs TX only) — pakon_replay --scan does not
                               verify reply bytes, so that is fine.
  ep6     EP6_FILE OUT.raw     strip the EP6L record headers and concatenate
                               payloads into a raw stream for pakon_image.py

Limitations (inherent to the capture, called out rather than guessed at):
  - RX payloads are absent (METHOD_NEITHER; the driver could not log them).
  - Vendor-request wLength is not in the input record (it came from the
    IOCTL's output buffer size); for IN requests we default to 32, the chunk
    size observed for the 0xA9 parameter-table reads on the F-135.
"""
import argparse
import signal
import struct
import sys

try:
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
except (AttributeError, ValueError):
    pass

# Kept in sync with include/pakon_proto.h.
ADDRESS_NAMES = {0x10: "AD_HOST", 0x20: "AD_PICL", 0x22: "AD_BOOT_PICL",
                 0x24: "AD_PICM", 0x26: "AD_BOOT_PICM", 0x40: "AD_PICL_PLUS",
                 0x42: "AD_BOOT_PICL_PLUS", 0x44: "AD_PICM_PLUS",
                 0x46: "AD_BOOT_PICM_PLUS"}

FRAME_TYPE_NAMES = {0x01: "READ", 0x02: "WRITE", 0x03: "READ_STATUS",
                    0x04: "CMD"}

IOCTL_NAMES = {
    0x222014: "VENDOR_REQUEST",
    0x222018: "GET_CURRENT_CONFIG",
    0x22201C: "ANCHOR_DOWNLOAD_0x200",
    0x222031: "RESET",
    0x222054: "GET_CURRENT_FRAME_NUMBER",
    0x222059: "VENDOR_OR_CLASS_REQUEST",
    0x22205C: "GET_LAST_ERROR",
    0x22206D: "ANCHOR_DOWNLOAD_0x40",
    0x222074: "GET_DRIVER_VERSION",
    0x222084: "SET_FEATURE",
    0x222088: "PAKON_READ_DIRECT",
    0x22208C: "PAKON_WRITE_DIRECT",
    0x222090: "PAKON_SEND_AND_RECEIVE_PACKET",
}
IOCTL_PPB_PACKET = 0x222090
IOCTL_VENDOR_OR_CLASS = 0x222059

# Command names inferred in the owner's RE notes (usb-protocol.md). Address
# matters: PICL and PICM reuse numbers for different meanings.
PICL_COMMAND_NAMES = {0x80: "SetCcdConfig", 0x81: "SetCcdGainOffset",
                      0x82: "SetColorMatrix", 0x83: "ReadCcdStatus",
                      0x84: "ReadLightStatus", 0x87: "SetLightPower",
                      0x88: "ReadTemperature", 0x89: "EnableScan",
                      0x8A: "AcquireLine", 0x8B: "SetCcdExposure_B",
                      0x8C: "SetCcdExposure_G", 0x8D: "SetCcdExposure_R",
                      0x8F: "SetLightConfig", 0x90: "ReadSensorData",
                      0x91: "SetScanLineParams", 0x92: "EndAcquisition",
                      0xD0: "SetTEC_1", 0xD1: "SetTEC_2"}
PICM_COMMAND_NAMES = {0x00: "ResetMotor", 0x82: "SetMotorSpeed",
                      0x84: "SetMotorConfig", 0x97: "InitMotor",
                      0xA0: "EngageFilmDrive", 0xA1: "StopFilmDrive",
                      0xA2: "DisengageFilmDrive", 0xA5: "SetMotorCalibration"}
HOST_COMMAND_NAMES = {0x84: "HostReady", 0x85: "HostReset",
                      0x8F: "HostSetMode"}

PLOG_MAGIC = b"PLOG"
EP6L_MAGIC = b"EP6L"
FILE_HEADER_LEN = 24
PLOG_RECORD_HEADER = struct.Struct("<qIBBiI")  # ts, code, dir, rsvd, status, len
EP6L_RECORD_HEADER = struct.Struct("<qI")      # ts, len


class PlogRecord:
    __slots__ = ("timestamp", "ioctl_code", "direction", "status", "payload")

    def __init__(self, timestamp, ioctl_code, direction, status, payload):
        self.timestamp = timestamp
        self.ioctl_code = ioctl_code
        self.direction = direction      # 'I' request or 'O' response
        self.status = status
        self.payload = payload


def read_file_header(handle, expected_magic, path):
    header = handle.read(FILE_HEADER_LEN)
    if len(header) < FILE_HEADER_LEN or header[:4] != expected_magic:
        sys.exit(f"{path}: not a {expected_magic.decode()} file")
    version, = struct.unpack_from("<H", header, 4)
    perf_freq, = struct.unpack_from("<q", header, 8)
    system_time, = struct.unpack_from("<q", header, 16)
    return version, perf_freq, system_time


def parse_plog(path):
    records = []
    with open(path, "rb") as handle:
        version, perf_freq, system_time = read_file_header(handle, PLOG_MAGIC,
                                                           path)
        while True:
            raw_header = handle.read(PLOG_RECORD_HEADER.size)
            if len(raw_header) < PLOG_RECORD_HEADER.size:
                break
            (timestamp, ioctl_code, direction, _reserved, status,
             payload_len) = PLOG_RECORD_HEADER.unpack(raw_header)
            payload = handle.read(payload_len)
            if len(payload) < payload_len:
                sys.exit(f"{path}: truncated record at EOF")
            records.append(PlogRecord(timestamp, ioctl_code, chr(direction),
                                      status, payload))
    return perf_freq, system_time, records


def frame_summary(payload):
    """One-line decode of a PPB wire frame [type][count][addr][plen][cmd]…"""
    if len(payload) < 3:
        return f"short frame {payload.hex()}"
    frame_type, frame_count, address = payload[0], payload[1], payload[2]
    type_name = FRAME_TYPE_NAMES.get(frame_type, f"type{frame_type:02x}")
    address_name = ADDRESS_NAMES.get(address, f"addr{address:02x}")
    if frame_type == 0x03:                       # READ_STATUS: no cmd byte
        return f"{type_name:11s} {address_name}"
    if len(payload) < 5:
        return f"{type_name:11s} {address_name} {payload[3:].hex()}"
    payload_len, command = payload[3], payload[4]
    if address in (0x20, 0x40):
        command_name = PICL_COMMAND_NAMES.get(command, f"0x{command:02x}")
    elif address in (0x24, 0x44):
        command_name = PICM_COMMAND_NAMES.get(command, f"0x{command:02x}")
    elif address == 0x10:
        command_name = HOST_COMMAND_NAMES.get(command, f"0x{command:02x}")
    else:
        command_name = f"0x{command:02x}"
    data_bytes = payload[5:]
    data_text = f"  [{data_bytes.hex(' ')}]" if data_bytes else ""
    return (f"{type_name:11s} {address_name:13s} {command_name}"
            f"(n={payload_len}){data_text}")


def vendor_request_fields(payload):
    """Decode VENDOR_OR_CLASS_REQUEST_CONTROL (10 bytes, from ezusb.h):
    direction, requestType, recipient, reservedBits, request, pad,
    value:u16le, index:u16le."""
    if len(payload) < 10:
        return None
    direction, request_type, recipient = payload[0], payload[1], payload[2]
    request = payload[4]
    value, index = struct.unpack_from("<HH", payload, 6)
    bm_request_type = ((0x80 if direction else 0x00) |
                       ((request_type & 3) << 5) | (recipient & 0x1f))
    return direction, bm_request_type, request, value, index


def dump(args):
    perf_freq, _system_time, records = parse_plog(args.ioctl_log)
    first_timestamp = records[0].timestamp if records else 0
    shown = 0
    for record in records:
        if record.direction != "I":
            continue        # response records carry no payload (TX-only log)
        seconds = (record.timestamp - first_timestamp) / perf_freq
        name = IOCTL_NAMES.get(record.ioctl_code,
                               f"IOCTL_{record.ioctl_code:06x}")
        if record.ioctl_code == IOCTL_PPB_PACKET:
            text = frame_summary(record.payload)
        elif record.ioctl_code == IOCTL_VENDOR_OR_CLASS:
            fields = vendor_request_fields(record.payload)
            if fields:
                direction, bm_request_type, request, value, index = fields
                text = (f"bmReqType={bm_request_type:02x} bReq={request:02x} "
                        f"wValue={value:04x} wIndex={index:04x} "
                        f"{'IN' if direction else 'OUT'}")
            else:
                text = record.payload.hex()
        else:
            text = record.payload[:32].hex(" ")
        if args.grep and args.grep not in text and args.grep not in name:
            continue
        print(f"[{seconds:10.6f}] {name:34s} {text}")
        shown += 1
    print(f"# {shown} request records shown "
          f"({len(records)} total incl. responses)", file=sys.stderr)


def read_ep6_packet_times(ep6_path):
    """Return the (timestamp, length) list of an EP6L file — headers only."""
    packets = []
    with open(ep6_path, "rb") as handle:
        read_file_header(handle, EP6L_MAGIC, ep6_path)
        while True:
            raw_header = handle.read(EP6L_RECORD_HEADER.size)
            if len(raw_header) < EP6L_RECORD_HEADER.size:
                break
            timestamp, payload_len = EP6L_RECORD_HEADER.unpack(raw_header)
            handle.seek(payload_len, 1)
            packets.append((timestamp, payload_len))
    return packets


def emit_pakscan(args):
    perf_freq, _system_time, records = parse_plog(args.ioctl_log)
    # EP6 image packets share the ioctl log's performance-counter clock, so
    # they interleave into the command stream as M lines by timestamp.
    image_packets = []
    for ep6_path in args.ep6 or []:
        image_packets.extend(read_ep6_packet_times(ep6_path))
    image_packets.sort()
    image_index = 0
    frame_lines = vendor_lines = image_lines = skipped = 0
    skipped_names = {}
    with open(args.out, "w") as script:
        script.write("# pakon scan operation script "
                     "(from FX35-datalogger ioctl_log.bin — F-135+)\n")
        script.write(f"# source: {args.ioctl_log}\n")
        script.write("# O <hex> = EP1 cmd out (+read reply);  M <n> = read n "
                     "image bytes;  C bmReqType bReq wValue wIndex wLen "
                     "[data]\n")
        script.write("# NOTE: replies were not captured (TX-only driver log); "
                     "replay must not byte-verify them.\n")
        for record in records:
            if record.direction != "I":
                continue
            while (image_index < len(image_packets) and
                   image_packets[image_index][0] <= record.timestamp):
                script.write(f"M {image_packets[image_index][1]}\n")
                image_index += 1
                image_lines += 1
            if record.ioctl_code == IOCTL_PPB_PACKET:
                script.write("O " + record.payload.hex() + "\n")
                frame_lines += 1
            elif record.ioctl_code == IOCTL_VENDOR_OR_CLASS:
                fields = vendor_request_fields(record.payload)
                if fields is None:
                    skipped += 1
                    continue
                direction, bm_request_type, request, value, index = fields
                trailing_data = record.payload[10:]
                if direction:
                    length = args.vendor_in_length
                else:
                    length = len(trailing_data)
                line = (f"C {bm_request_type:02x} {request:02x} {value:04x} "
                        f"{index:04x} {length:04x}")
                if trailing_data:
                    line += " " + trailing_data.hex()
                script.write(line + "\n")
                vendor_lines += 1
            else:
                name = IOCTL_NAMES.get(record.ioctl_code,
                                       f"IOCTL_{record.ioctl_code:06x}")
                skipped_names[name] = skipped_names.get(name, 0) + 1
                skipped += 1
        while image_index < len(image_packets):
            script.write(f"M {image_packets[image_index][1]}\n")
            image_index += 1
            image_lines += 1
    print(f"wrote {args.out}: {frame_lines} commands, "
          f"{vendor_lines} control transfers, {image_lines} image reads")
    if skipped:
        detail = ", ".join(f"{name}×{count}"
                           for name, count in sorted(skipped_names.items()))
        print(f"skipped {skipped} non-replayable records: {detail}")


def extract_ep6(args):
    total_bytes = packet_count = 0
    size_histogram = {}
    with open(args.ep6_file, "rb") as source, open(args.out, "wb") as sink:
        _version, perf_freq, _system_time = read_file_header(source,
                                                             EP6L_MAGIC,
                                                             args.ep6_file)
        first_timestamp = last_timestamp = None
        while True:
            raw_header = source.read(EP6L_RECORD_HEADER.size)
            if len(raw_header) < EP6L_RECORD_HEADER.size:
                break
            timestamp, payload_len = EP6L_RECORD_HEADER.unpack(raw_header)
            payload = source.read(payload_len)
            if len(payload) < payload_len:
                sys.exit(f"{args.ep6_file}: truncated packet at EOF")
            sink.write(payload)
            total_bytes += payload_len
            packet_count += 1
            size_histogram[payload_len] = size_histogram.get(payload_len,
                                                             0) + 1
            if first_timestamp is None:
                first_timestamp = timestamp
            last_timestamp = timestamp
    duration = ((last_timestamp - first_timestamp) / perf_freq
                if packet_count > 1 else 0.0)
    sizes = ", ".join(f"{size}×{count}" for size, count
                      in sorted(size_histogram.items(), reverse=True)[:6])
    print(f"wrote {args.out}: {total_bytes} bytes in {packet_count} packets "
          f"over {duration:.1f}s (packet sizes: {sizes})")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.
                                     RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    dump_command = commands.add_parser("dump",
                                       help="decoded IOCTL timeline")
    dump_command.add_argument("ioctl_log")
    dump_command.add_argument("--grep", help="only lines containing this text")
    dump_command.set_defaults(func=dump)

    pakscan_command = commands.add_parser("pakscan",
                                          help="convert to .pakscan script")
    pakscan_command.add_argument("ioctl_log")
    pakscan_command.add_argument("out")
    pakscan_command.add_argument("--ep6", nargs="*", metavar="EP6_FILE",
                                 help="EP6 capture files from the same "
                                      "session; their packets interleave as "
                                      "M lines by shared timestamp")
    pakscan_command.add_argument("--vendor-in-length", type=int, default=32,
                                 help="wLength for IN vendor requests (not in "
                                      "the log; default 32 per the F-135 "
                                      "0xA9 table reads)")
    pakscan_command.set_defaults(func=emit_pakscan)

    ep6_command = commands.add_parser("ep6",
                                      help="EP6L capture -> raw image stream")
    ep6_command.add_argument("ep6_file")
    ep6_command.add_argument("out")
    ep6_command.set_defaults(func=extract_ep6)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()

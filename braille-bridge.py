#!/usr/bin/env python3
"""
Bridge between ESP32 BrailleWave BLE HID adapter and brltty.

The ESP32 exposes HandyTech serial protocol wrapped in HID reports:
  Report 0x01 (input/GET):  [length] [HT serial bytes...] — data from BrailleWave
  Report 0x02 (output/SET): [length] [HT serial bytes...] — data to BrailleWave
  Report 0xFB (output/SET): [command] — firmware command (0x01 = flush)
  Report 0xFC (input/GET):  [report_id] [major] [minor] — firmware version

brltty's ht driver expects raw HT serial on a serial-like device.
This bridge creates a PTY pair and shuttles data between hidraw and the PTY.

Usage:
    sudo python3 braille-bridge.py [hidraw_device]
    # Then in another terminal:
    sudo brltty -b ht -d serial:$PTY_PATH -n -e
"""

import os
import sys
import pty
import select
import struct
import time
import glob

HT_HID_DATA_SIZE = 64
RPT_HT_OUT_DATA = 0x01
RPT_HT_IN_DATA = 0x02

def find_braillewave_hidraw():
    """Find the hidraw device for the BrailleWave HT USB-HID interface.

    The ESP32 creates multiple hidraw devices:
    - One for the standard Braille HID collection (Usage Page 0x41)
    - One or more for the HT USB-HID vendor collection
    We want the LAST one with HT vendor ID (1FE4:0003) — that's the
    vendor-defined collection with the HT serial tunnel reports.
    """
    ht_devs = []
    for hidraw in sorted(glob.glob('/sys/class/hidraw/hidraw*/device/uevent')):
        try:
            with open(hidraw) as f:
                content = f.read()
            if 'BrailleWave' not in content:
                continue
            if '00001FE4' in content:
                dev = '/dev/' + hidraw.split('/')[4]
                ht_devs.append(dev)
        except:
            continue

    # Return the last (highest-numbered) HT device — that's the vendor collection
    if ht_devs:
        return ht_devs[-1]

    # Fallback: last BrailleWave hidraw
    devs = []
    for hidraw in sorted(glob.glob('/sys/class/hidraw/hidraw*/device/uevent')):
        try:
            with open(hidraw) as f:
                if 'BrailleWave' in f.read():
                    devs.append('/dev/' + hidraw.split('/')[4])
        except:
            pass
    return devs[-1] if devs else None


def main():
    if len(sys.argv) > 1:
        hidraw_path = sys.argv[1]
    else:
        hidraw_path = find_braillewave_hidraw()
        if not hidraw_path:
            print("ERROR: No BrailleWave hidraw device found", file=sys.stderr)
            print("Usage: sudo python3 braille-bridge.py /dev/hidrawN", file=sys.stderr)
            sys.exit(1)

    print(f"Using HID device: {hidraw_path}")

    # Open hidraw
    try:
        hid_fd = os.open(hidraw_path, os.O_RDWR | os.O_NONBLOCK)
    except PermissionError:
        print(f"ERROR: Permission denied on {hidraw_path}. Run with sudo.", file=sys.stderr)
        sys.exit(1)

    # Create PTY pair
    master_fd, slave_fd = pty.openpty()
    slave_name = os.ttyname(slave_fd)
    # Make a predictable symlink
    link_path = '/tmp/braillewave'
    try:
        os.unlink(link_path)
    except FileNotFoundError:
        pass
    os.symlink(slave_name, link_path)

    print(f"PTY slave: {slave_name}")
    print(f"Symlink:   {link_path}")
    print()
    print(f"Start brltty with:")
    print(f"  sudo brltty -b ht -d {link_path} -n -e")
    print()
    print("Bridge running... (Ctrl+C to stop)")

    try:
        while True:
            # Wait for data on either side
            readable, _, _ = select.select([hid_fd, master_fd], [], [], 0.05)

            for fd in readable:
                if fd == hid_fd:
                    # Data from HID (BrailleWave → brltty via ESP32)
                    try:
                        data = os.read(hid_fd, HT_HID_DATA_SIZE + 2)
                        if len(data) >= 3:
                            # hidraw prepends report ID:
                            # [report_id] [length] [HT serial bytes...] [padding...]
                            report_id = data[0]
                            if report_id == RPT_HT_OUT_DATA:
                                length = data[1]
                                if length > 0 and len(data) >= 2 + length:
                                    ht_bytes = data[2:2+length]
                                    os.write(master_fd, ht_bytes)
                    except BlockingIOError:
                        pass

                elif fd == master_fd:
                    # Data from brltty → HID (to BrailleWave via ESP32)
                    try:
                        data = os.read(master_fd, 256)
                        if data:
                            # Wrap in HT USB-HID report 0x02
                            # hidraw format: [report_id] [payload...] (total = 1 + report_size)
                            # Our report 0x02 is 64 bytes, so write 65 total
                            offset = 0
                            while offset < len(data):
                                chunk = data[offset:offset + HT_HID_DATA_SIZE - 1]
                                report = bytearray(HT_HID_DATA_SIZE + 1)  # 65 bytes
                                report[0] = RPT_HT_IN_DATA  # report ID
                                report[1] = len(chunk)       # length byte
                                report[2:2+len(chunk)] = chunk
                                os.write(hid_fd, bytes(report))
                                offset += len(chunk)
                    except BlockingIOError:
                        pass

    except KeyboardInterrupt:
        print("\nStopping bridge...")
    finally:
        os.close(hid_fd)
        os.close(master_fd)
        os.close(slave_fd)
        try:
            os.unlink(link_path)
        except:
            pass


if __name__ == '__main__':
    main()

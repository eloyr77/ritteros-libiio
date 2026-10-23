#!/usr/bin/env python3
"""A pretend ADALM-Pluto for the libiio tests: an iiod on the Mac
speaking iiod's 0.x text protocol, with a device description of our own
making (Pluto-like: an AD9363's phy, its receive and transmit cores, the
FPGA's monitor, a trigger, a hardware monitor). QEMU's user network takes
the guest's 10.0.2.2:PORT here.

Usage:
  testiiod.py serve PORT [ADDRESS]
                               serve until killed (on 127.0.0.1, or ADDRESS:
                               0.0.0.0 for a real network); every command is
                               logged to stdout, one line each
  testiiod.py xml              print the description (what PRINT sends)
  testiiod.py expect IP        print what the test program iiotest must
                               print for this server (seen at IP)

The receive core (cf-ad9361-lpc) streams I = (37 k mod 4093) - 2046 and
Q = 2046 - (53 k mod 4093) for scan k = 0, 1, ... of each OPEN, as 16-bit
little-endian samples (12 bits sign-extended, as the Pluto's; 4093 so that
a second refill does not repeat the first). What the
transmit core is sent is logged with its SHA-256.
"""
import hashlib
import socket
import struct
import sys
import threading

VERSION = "0.26.0000000"
SCANS_RX = "cf-ad9361-lpc"

CONTEXT_ATTRS = [
    ("hw_model", "RitterOS test Pluto (Z7010-AD9363A)"),
    ("hw_model_variant", "1"),
    ("hw_serial", "0123456789abcdef0123456789abcdef"),
    ("fw_version", "v0.3.24-test"),
    ("ad9361-phy,model", "ad9363a"),
    ("local,kernel", "6.1.0-testiiod"),
]
DESCRIPTION = "10.0.2.2 Linux testiiod 6.1.0-testiiod armv7l"


def chan(cid, kind, attrs, name=None, scan=None):
    return {"id": cid, "type": kind, "name": name, "attrs": attrs, "scan": scan}


# Each device: id, name, channels, device/debug/buffer attributes with their
# first values. Channel attribute values are keyed (kind, id, attr).
DEVICES = [
    {"id": "iio:device0", "name": "ad9361-phy",
     "channels": [
         chan("altvoltage1", "output", [("frequency", "2450000000"), ("powerdown", "0"),
                                        ("external", "0")], name="TX_LO"),
         chan("voltage0", "input", [("hardwaregain", "71.000000 dB"),
                                    ("gain_control_mode", "slow_attack"),
                                    ("gain_control_mode_available", "manual fast_attack slow_attack hybrid"),
                                    ("rf_bandwidth", "18000000"), ("sampling_frequency", "30720000"),
                                    ("rssi", "97.25 dB"), ("rf_port_select", "A_BALANCED")]),
         chan("voltage0", "output", [("hardwaregain", "-10.000000 dB"), ("rf_bandwidth", "18000000"),
                                     ("sampling_frequency", "30720000"), ("rf_port_select", "A")]),
         chan("altvoltage0", "output", [("frequency", "2400000000"),
                                        ("frequency_available", "[70000000 1 6000000000]"),
                                        ("powerdown", "0"), ("fastlock_recall", "")], name="RX_LO"),
         chan("temp0", "input", [("input", "35000")]),
     ],
     "attrs": [("calib_mode", "auto"), ("ensm_mode", "fdd"),
               ("ensm_mode_available", "sleep wait alert fdd pinctrl pinctrl_fdd_indep"),
               ("xo_correction", "40000000"), ("filter_fir_config", "FIR Rx: 0,0 Tx: 0,0")],
     "debug": [("direct_reg_access", "0x0"), ("loopback", "0"), ("digital_tune_skip", "0")],
     "buffer": []},
    {"id": "iio:device1", "name": "xadc",
     "channels": [
         # (scan elements in other formats, for the conversions: never streamed)
         chan("temp0", "input", [("raw", "2500"), ("offset", "-2219"), ("scale", "123.040771")],
              scan=(0, "be:u12/16>>4")),
         chan("voltage0", "input", [("raw", "1300"), ("scale", "0.732421")], name="vccint",
              scan=(1, "le:s12/16>>4")),
     ],
     "attrs": [("sampling_frequency", "961538")], "debug": [], "buffer": []},
    {"id": "iio:device2", "name": "cf-ad9361-dds-core-lpc",
     "channels": [
         chan("voltage0", "output", [("calibphase", "0.000000"), ("calibscale", "1.000000"),
                                     ("sampling_frequency", "30720000")],
              scan=(0, "le:S16/16>>0")),
         chan("voltage1", "output", [("calibphase", "0.000000"), ("calibscale", "1.000000"),
                                     ("sampling_frequency", "30720000")],
              scan=(1, "le:S16/16>>0")),
         chan("altvoltage0", "output", [("frequency", "9279985"), ("raw", "1"), ("scale", "0.000000")],
              name="TX1_I_F1"),
     ],
     "attrs": [], "debug": [("direct_reg_access", "0x0")],
     "buffer": [("data_available", "0"), ("watermark", "2048")]},
    {"id": "iio:device3", "name": SCANS_RX,
     "channels": [
         # (voltage1 first: the library orders scan elements by index)
         chan("voltage1", "input", [("calibbias", "0"), ("calibphase", "0.000000"),
                                    ("calibscale", "1.000000"), ("samples_pps", "0"),
                                    ("sampling_frequency", "30720000")],
              scan=(1, "le:S12/16>>0")),
         chan("voltage0", "input", [("calibbias", "0"), ("calibphase", "0.000000"),
                                    ("calibscale", "1.000000"), ("samples_pps", "0"),
                                    ("sampling_frequency", "30720000")],
              scan=(0, "le:S12/16>>0")),
     ],
     "attrs": [], "debug": [("direct_reg_access", "0x0")],
     "buffer": [("data_available", "0"), ("watermark", "2048")]},
    {"id": "trigger0", "name": "test-trigger", "channels": [], "attrs": [("sampling_frequency", "1000")],
     "debug": [], "buffer": []},
    {"id": "hwmon0", "name": "ad9361_hwmon",
     "channels": [chan("temp1", "input", [("input", "41000")])],
     "attrs": [], "debug": [], "buffer": []},
]

# Values that fail on reading (iiod passes the driver's error): -EINVAL.
FAILING = {("iio:device0", "OUTPUT", "altvoltage0", "fastlock_recall")}


def escape(text):
    return (text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
            .replace('"', "&quot;"))


def sysfs_name(dev, ch, attr):
    prefix = "out" if ch["type"] == "output" else "in"
    name = f"_{ch['name']}" if ch["name"] else ""
    return f"{prefix}_{ch['id']}{name}_{attr}"


def description_xml():
    out = ['<?xml version="1.0" encoding="utf-8"?>',
           '<!DOCTYPE context [<!ELEMENT context (device | context-attribute)*>'
           '<!ELEMENT context-attribute EMPTY><!ELEMENT device (channel | attribute | '
           'debug-attribute | buffer-attribute)*><!ATTLIST context name CDATA #REQUIRED '
           'description CDATA #IMPLIED><!ATTLIST channel id CDATA #REQUIRED type '
           '(input|output) #REQUIRED name CDATA #IMPLIED>]>',
           f'<context name="local" description="{escape(DESCRIPTION)}" >']
    for k, v in CONTEXT_ATTRS:
        out.append(f'<context-attribute name="{escape(k)}" value="{escape(v)}" />')
    for dev in DEVICES:
        name = f' name="{escape(dev["name"])}"' if dev["name"] else ""
        out.append(f'<device id="{escape(dev["id"])}"{name} >')
        for ch in dev["channels"]:
            cname = f' name="{escape(ch["name"])}"' if ch["name"] else ""
            out.append(f'<channel id="{ch["id"]}"{cname} type="{ch["type"]}" >')
            if ch["scan"]:
                out.append(f'<scan-element index="{ch["scan"][0]}" format="{escape(ch["scan"][1])}" />')
            for a, _ in ch["attrs"]:
                out.append(f'<attribute name="{a}" filename="{sysfs_name(dev, ch, a)}" />')
            out.append('</channel>')
        for a, _ in dev["attrs"]:
            out.append(f'<attribute name="{a}" />')
        for a, _ in dev["buffer"]:
            out.append(f'<buffer-attribute name="{a}" />')
        for a, _ in dev["debug"]:
            out.append(f'<debug-attribute name="{a}" />')
        out.append('</device>')
    out.append('</context>')
    return "".join(out)


def rx_sample(k):
    """Scan k of the receive core: I and Q."""
    return (37 * k) % 4093 - 2046, 2046 - (53 * k) % 4093


def rx_scan_order(dev):
    """The receive core's scan elements by index: [(index, channel id)]."""
    return sorted((ch["scan"][0], ch["id"]) for ch in dev["channels"] if ch["scan"])


class Server:
    def __init__(self):
        self.lock = threading.Lock()
        self.values = {}
        self.regs = {}
        self.reg_addr = {}
        self.triggers = {}
        for dev in DEVICES:
            for a, v in dev["attrs"]:
                self.values[(dev["id"], "", "", a)] = v
            for a, v in dev["debug"]:
                self.values[(dev["id"], "DEBUG", "", a)] = v
            for a, v in dev["buffer"]:
                self.values[(dev["id"], "BUFFER", "", a)] = v
            for ch in dev["channels"]:
                kind = "OUTPUT" if ch["type"] == "output" else "INPUT"
                for a, v in ch["attrs"]:
                    self.values[(dev["id"], kind, ch["id"], a)] = v

    def device(self, did):
        for dev in DEVICES:
            if dev["id"] == did:
                return dev
        return None


def say(text):
    print(text, flush=True)


def handle(conn, server):
    f = conn.makefile("rb")
    opened = None            # (device, samples, mask words, cyclic, scan counter)

    def send(data):
        conn.sendall(data if isinstance(data, bytes) else data.encode())

    def answer(n):
        send(f"{n}\n")

    while True:
        raw = f.readline()
        if not raw:
            return
        line = raw.decode(errors="replace").strip()
        if not line:
            continue
        words = line.split()
        cmd = words[0].upper()
        if cmd != "READBUF":
            say(f"testiiod: {line}")
        if cmd == "EXIT":
            return
        if cmd == "VERSION":
            send(VERSION + "\n")
        elif cmd == "PRINT":
            xml = description_xml().encode()
            answer(len(xml))
            send(xml + b"\n")
        elif cmd == "TIMEOUT":
            answer(0)
        elif cmd in ("READ", "WRITE"):
            rest = words[1:]
            length = None
            if cmd == "WRITE":
                length = int(rest[-1])
                rest = rest[:-1]
            did = rest[0]
            if len(rest) == 4 and rest[1] in ("INPUT", "OUTPUT"):
                key = (did, rest[1], rest[2], rest[3])
            elif len(rest) == 3 and rest[1] in ("DEBUG", "BUFFER"):
                key = (did, rest[1], "", rest[2])
            elif len(rest) == 2:
                key = (did, "", "", rest[1])
            else:
                if length is not None:
                    f.read(length)
                answer(-22)
                continue
            if cmd == "WRITE":
                data = f.read(length).rstrip(b"\0").decode(errors="replace").strip()
                with server.lock:
                    if key not in server.values:
                        answer(-2)
                        continue
                    if key[3] == "direct_reg_access":
                        parts = data.split()
                        addr = int(parts[0], 0)
                        if len(parts) > 1:
                            server.regs[(did, addr)] = int(parts[1], 0)
                        server.reg_addr[did] = addr
                    else:
                        server.values[key] = data
                say(f"testiiod: wrote {key[3]} = '{data}'")
                answer(length)
            else:
                with server.lock:
                    if key not in server.values:
                        answer(-2)
                        continue
                    if key in FAILING:
                        answer(-22)
                        continue
                    if key[3] == "direct_reg_access":
                        addr = server.reg_addr.get(did, 0)
                        value = f"0x{server.regs.get((did, addr), 0):X}"
                    else:
                        value = server.values[key]
                data = value.encode()
                answer(len(data))
                send(data + b"\n")
        elif cmd == "GETTRIG":
            name = server.triggers.get(words[1], "")
            if not name:
                answer(0)
            else:
                answer(len(name))
                send(name + "\n")
        elif cmd == "SETTRIG":
            if len(words) > 2:
                trig = server.device(words[2])
                server.triggers[words[1]] = trig["name"] if trig else ""
            else:
                server.triggers.pop(words[1], None)
            answer(0)
        elif cmd == "SET":
            answer(0)
        elif cmd == "OPEN":
            dev = server.device(words[1])
            if not dev:
                answer(-19)
                continue
            mask = int(words[3], 16)
            opened = [dev, int(words[2]), words[3], len(words) > 4 and words[4] == "CYCLIC", 0]
            say(f"testiiod: open {dev['name']} mask {mask:x}")
            answer(0)
        elif cmd == "CLOSE":
            opened = None
            answer(0)
        elif cmd == "READBUF":
            if not opened:
                answer(-9)
                continue
            dev, _, mask_text, _, k = opened
            want = int(words[2])
            mask = int(mask_text, 16)
            order = [cid for idx, cid in rx_scan_order(dev) if mask >> idx & 1]
            scan_bytes = 2 * len(order)
            scans = want // scan_bytes
            data = bytearray()
            for s in range(scans):
                i, q = rx_sample(k + s)
                for cid in order:
                    data += struct.pack("<h", i if cid == "voltage0" else q)
            opened[4] = k + scans
            say(f"testiiod: READBUF {words[1]} {want} ({scans} scans)")
            # (in two pieces, as iiod may send them; the mask with the first)
            half = (scans // 2) * scan_bytes
            answer(half)
            send(mask_text + "\n")
            send(bytes(data[:half]))
            answer(len(data) - half)
            send(bytes(data[half:]))
        elif cmd == "WRITEBUF":
            n = int(words[2])
            answer(n)
            data = f.read(n)
            say(f"testiiod: WRITEBUF {words[1]} {len(data)} bytes sha256 "
                f"{hashlib.sha256(data).hexdigest()}")
            answer(len(data))
        else:
            answer(-22)


def serve(port, address="127.0.0.1"):
    server = Server()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((address, port))
    s.listen(8)
    say(f"testiiod: listening on {port}")
    while True:
        conn, _ = s.accept()
        threading.Thread(target=lambda c=conn: (handle(c, server), c.close()), daemon=True).start()


def parse_format(text):
    """'le:S12/16>>0' -> (bits, length, shift, signed, be, fully defined, repeat)."""
    sign, rest = text[3], text[4:]
    bits, rest = rest.split("/", 1)
    length, shift = rest.split(">>")
    repeat = 1
    if "X" in length:
        length, repeat = length.split("X")
    bits, length = int(bits), int(length)
    return (bits, length, int(shift), int(sign in "sS"), int(text[0] == "b"),
            int(sign in "SU" or bits == length), int(repeat))


def errtext(name):
    """RitterOS's strerror texts (user/lib/string.c) for what the test meets."""
    return {"EIO": "Input/output error", "EINVAL": "Invalid argument",
            "ENOENT": "No such file or directory", "EBUSY": "Device or resource busy",
            "EBADF": "Bad file descriptor", "ENOSYS": "Function not implemented"}[name]


def expect(ip):
    """Every line iiotest (tests/iiotest.c) prints against this server at
    ip, from the description and the sample model above."""
    t = "iiotest: "
    out = [t + "library 0.26 (ritter), backends 2: ip xml, has ip 1 usb 0",
           t + "usb: refused, errno 38",
           t + f"context 'network', backend 0.26 ({VERSION.split('.')[2]}), {len(DEVICES)} devices",
           t + f"description '{DESCRIPTION}'",
           t + f"{len(CONTEXT_ATTRS) + 2} attributes, hw_model '{dict(CONTEXT_ATTRS)['hw_model']}', "
               f"ip,ip-addr '{ip}'"]
    for d in DEVICES:
        trig = int(not d["channels"] and bool(d["name"]) and d["id"].startswith("trigger"))
        out.append(t + f"{d['id']} '{d['name']}': {len(d['channels'])} channels, "
                       f"{len(d['attrs'])} attributes, {len(d['buffer'])} buffer, "
                       f"{len(d['debug'])} debug, trigger {trig} hwmon {int(d['id'][0] == 'h')}")
    rx = [d for d in DEVICES if d["name"] == SCANS_RX][0]
    for n, (idx, cid) in enumerate(rx_scan_order(rx)):
        fmt = [c["scan"][1] for c in rx["channels"] if c["id"] == cid][0]
        b, l, sh, sg, be, full, rep = parse_format(fmt)
        out.append(t + f"rx channel {n}: {cid} index {idx}, {b}/{l}>>{sh} signed {sg} be {be} "
                       f"full {full} repeat {rep}")
    # (enum values: IIO_ALTVOLTAGE 15, IIO_VOLTAGE 0, HWMON_TEMP 3)
    out.append(t + "RX_LO is altvoltage0 (output), type 15 modifier 0; voltage0 type 0; hwmon temp1 type 3")
    phy = DEVICES[0]
    lo = [c for c in phy["channels"] if c["name"] == "RX_LO"][0]
    out.append(t + f"RX_LO frequency file {sysfs_name(phy, lo, 'frequency')}")
    out.append(t + "identify: 0 altvoltage1 frequency")
    out.append(t + "identify: 0 - ensm_mode")
    out.append(t + f"LO 0 {dict(lo['attrs'])['frequency']}")
    out.append(t + "LO set 0, now 100000000")
    out.append(t + "gain 0 71.00")                       # ("71.000000 dB")
    out.append(t + "tx gain set 0, now -20.25")
    out.append(t + "powerdown 0 0, now 1")
    out.append(t + "gain mode wrote 7, now 'manual'")    # (with its 0, as libiio)
    out.append(t + "ensm_mode 3 'fdd'")
    out.append(t + f"ensm_mode in 3 bytes: {errtext('EIO')}")
    out.append(t + "after it, calib_mode 4 'auto'")
    out.append(t + f"fastlock_recall: {errtext('EINVAL')}")
    out.append(t + f"nonexistent: {errtext('ENOENT')}")
    out.append(t + "rx buffer watermark 4 '2048'")
    out.append(t + "  frequency = '100000000' (9)")
    out.append(t + f"  frequency_available = '{dict(lo['attrs'])['frequency_available']}' "
                   f"({len(dict(lo['attrs'])['frequency_available'])})")
    out.append(t + "  powerdown = '1' (1)")              # (fastlock_recall fails: left out)
    out.append(t + "read_all 0: 3 values")
    out.append(t + "register 0x37: write 0 read 0 value 0x5A")
    out.append(t + "trigger 0 none, set 0, get 0 test-trigger")
    out.append(t + "kernel buffers 0, timeout 0")
    # S12/16>>0 is fully defined (sign-extended by the hardware): no masking.
    out.append(t + "convert 0fff:4095 0800:2048 07ff:2047 f800:-2048 0001:1, -1 back 0fff")
    out.append(t + "convert s12>>4 fff0:-1 7f10:2033, be u12>>4 abcd:2748; back -5 b0ff 0abc abc0")
    out.append(t + f"sample size before 0, buffer with no channels: {errtext('EINVAL')}")
    out.append(t + "enabled 1 1, sample size 4")
    out.append(t + f"second buffer: {errtext('EBUSY')}")
    n = 4096
    i0, q0 = rx_sample(0)
    out.append(t + f"refill {4 * n}, first I {i0} Q {q0} at offsets 0 2")
    sha_i = hashlib.sha256(b"".join(struct.pack("<h", rx_sample(k)[0]) for k in range(n))).hexdigest()
    sha_q = hashlib.sha256(b"".join(struct.pack("<h", rx_sample(k)[1]) for k in range(n))).hexdigest()
    out.append(t + f"rx {n} scans, step 4, I sha256 {sha_i}")
    out.append(t + f"rx Q sha256 {sha_q}")
    i2, q2 = rx_sample(n)
    out.append(t + f"refill again {4 * n}, first I {i2} Q {q2}")
    out.append(t + f"I alone: size 2, refill 16, 16 bytes, {rx_sample(0)[0]} {rx_sample(1)[0]}, step 2")
    wave_i = [(k * 31 - 16000) for k in range(1024)]
    tx = b"".join(struct.pack("<hh", v, -v) for v in wave_i)
    out.append(t + f"tx wrote 2048 2048, pushed 4096, sha256 {hashlib.sha256(tx).hexdigest()}")
    out.append(t + f"tx partial 400, sha256 {hashlib.sha256(tx[:400]).hexdigest()}")
    out.append(t + f"tx refill: {errtext('EINVAL')}")
    out.append(t + f"cyclic push 1024, again: {errtext('EBUSY')}")
    out.append(t + f"after cancel: {errtext('EBADF')}")
    out.append(t + f"xml copy 'xml' {len(DEVICES)} devices; clone 'network' {len(DEVICES)} devices, "
                   "LO 100000000")
    out.append(t + f"xml copy read: {errtext('ENOSYS')}")
    out.append(t + f"broken xml: {errtext('EINVAL')}")
    out.append(t + "done")
    print("\n".join(out))


def main():
    if len(sys.argv) in (3, 4) and sys.argv[1] == "serve":
        serve(int(sys.argv[2]), *sys.argv[3:])
    elif sys.argv[1:] == ["xml"]:
        print(description_xml())
    elif len(sys.argv) == 3 and sys.argv[1] == "expect":
        expect(sys.argv[2])
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main()

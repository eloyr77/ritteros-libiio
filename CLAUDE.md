# CLAUDE.md

libiio for RitterOS: Analog Devices' libiio interface (the classic one,
version 0.26) for programs on RitterOS that talk to an ADALM-Pluto (here a
PlutoNano with F5OEO's Tezuka firmware, which runs libiio/iiod 0.26) or
another IIO device behind an `iiod`. Our own code, written from iiod's text
protocol (libiio itself is LGPL-2.1; nothing of it is copied here). Built
with RitterOS's SDK: read `RITTEROS/sdk/CLAUDE.md` first (the toolchain,
`make`, ssh to a RitterOS machine, what RitterOS's C library has).

## Layout

| Path | What |
|---|---|
| `include/iio.h` | the interface: libiio 0.26's names, types and behaviour |
| `src/iio.c` | contexts, devices, channels, attributes, sample conversion, buffers |
| `src/iioxml.c` | the context's XML description (what iiod's PRINT sends) |
| `src/iionet.c` | the iiod client: one TCP connection for attributes, one per buffer |
| `src/iiopriv.h` | the library's insides (not for programs) |
| `tools/iio_info.c`, `tools/iio_attr.c` | libiio's tools (their output as libiio's) |
| `tests/iiotest.c` | a test program going through the whole interface |
| `tests/testiiod.py` | a pretend Pluto: an iiod with a made-up description and a model of its samples |
| `tests/check.sh` | runs iiotest against the pretend Pluto and checks every line (`make check-host`, `make check`) |
| `tests/host/ritteros.h` | a stand-in for RitterOS's header, so the library builds on a Mac |

## Building and sending to a RitterOS machine

RitterOS's source must be checked out next to this project (`../RitterOS`)
or given with `RITTEROS=/path`; its toolchain (`scripts/setup-toolchain.sh`
there) or Zig 0.17.0 compiles.

```sh
make                                          # build/x86_64/: libiio.a, iio_info, iio_attr, iiotest
make ARCH=aarch64                             # the same for 64-bit ARM
make install HOST=root@ADDRESS                # iio_info, iio_attr -> /system/ARCH/bin, iiotest -> /system/ARCH/test
make install HOST=root@ADDRESS SSH_OPTS="-i ~/.ssh/ritteros"
```

Programs are linked statically by default (`LINK ?= static`): they carry
the C library code they use, so they run on an installed system older than
the RitterOS checkout (libiio uses `strtol`, `strtod`, ... which RitterOS's
shared C library has only since its step 22m). `ssh` must log in with a key
(BatchMode): the machine's `/home/root/.ssh/authorized_keys`.

A program of your own: compile with `-I include` (plus the SDK's flags) and
link `build/ARCH/libiio.a` before RitterOS's libraries; the Makefile's
`$(IIO_ATTR)` rule is the pattern.

## Checking

```sh
make check-host                               # the library and iiotest on this Mac (ASan, UBSan)
make check HOST=root@ADDRESS IIOD_FROM=MAC    # iiotest, iio_info, iio_attr on the RitterOS machine
```

Both start `tests/testiiod.py` here (port 47791, `IIOD_PORT=`). `IIOD_FROM`
is this Mac's address as the RitterOS machine sees it (10.0.2.2 from QEMU's
user network; on a real network the Mac's LAN address, and the server then
listens on every interface). Every line iiotest prints must equal what
`testiiod.py expect ADDRESS` prints: that model is the reference, written
apart from the C code. Change the library, then make both checks pass;
change the model only when the pretend Pluto itself changes.

## The real Pluto

- It is reached over its USB network: RitterOS has a CDC-NCM driver
  (`eth0`, ...), not RNDIS, which is Tezuka's default. On the Pluto:
  `fw_setenv usb_ethernet_mode ncm`, then restart it. The Pluto gives the
  host an address by DHCP and is `192.168.2.1` (`ip:192.168.2.1`).
- Tezuka's USB device is composite (NCM, mass storage, a serial console,
  IIO over USB, perhaps audio): the first real device for RitterOS's NCM
  driver, so problems there are RitterOS's (kernel/usbnet.c), not this
  library's.
- Only the network backend exists (`ip:HOST[:PORT]`, and `xml:FILE` for a
  saved description); no `usb:`, no local devices, no discovery (DNS-SD):
  give the address.

## Facts the code depends on

- Channels are ordered as libiio orders them: scan elements by index (equal
  ones by shift), the others after in the XML's order. A channel's number is
  its place there, and buffer masks (OPEN, READBUF's answer) count by it.
- A format's capital `S`/`U` means fully defined (already sign-extended by
  the hardware): no masking. The Pluto's samples are `le:S12/16>>0`.
- iiod answers READ with the length, the value and a `\n`. A value longer
  than the caller's buffer must be read past, or the connection is out of
  step. READBUF's answer comes in pieces, the mask only with the first; it
  ends when the count asked for has come.
- Error numbers on the wire are Linux's, which are RitterOS's too (`ENXIO`
  is defined in iiopriv.h: RitterOS's errno.h lacks it).
- RitterOS has no futex: the connection's lock is a ticket lock that sleeps
  a millisecond between looks. Waits on the server are cut into 100 ms
  slices so that `iio_buffer_cancel` from another thread is noticed.

## Still to come

libad9361 (`ad9361.h`: `ad9361_set_bb_rate` and the FIR helpers) in this
repository. Its preset filter coefficients are ADI's (LGPL): whether to
design our own taps or use theirs is the owner's decision.

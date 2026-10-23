#!/bin/bash
# The library's checks against the pretend Pluto (tests/testiiod.py), which
# runs here for the length of the check.
#
#   tests/check.sh host PROGRAM ADDRESS PORT
#       iiotest built for this Mac (make check-host), the server on ADDRESS
#   tests/check.sh ritteros HOST FROM PORT ARCH [SSH OPTIONS...]
#       iiotest, iio_info and iio_attr on the RitterOS machine HOST (user@address,
#       installed there by make install); FROM is this Mac's address as that
#       machine sees it (10.0.2.2 from QEMU's user network)
#
# Every line iiotest prints must be the model's (testiiod.py expect FROM);
# the samples it sends must reach the server with the model's checksums.
# Prints PASS/FAIL lines; exits 1 if anything failed.
set -u
cd "$(dirname "$0")/.." || exit 1
mode=$1
failures=0
work=build/check
mkdir -p "$work"
log="$work/testiiod-$mode.log"

pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; failures=$((failures + 1)); }

# compare WHAT EXPECTED GOT
compare() {
    if [ "$2" = "$3" ]; then
        pass "$1"
    else
        fail "$1"
        diff <(printf '%s\n' "$2") <(printf '%s\n' "$3") | sed 's/^/      /'
    fi
}

start_server() {
    python3 -I tests/testiiod.py serve "$1" "$2" >"$log" 2>&1 &
    server=$!
    for _ in $(seq 40); do
        grep -q listening "$log" 2>/dev/null && return
        sleep 0.25
    done
    echo "tests/check.sh: the pretend Pluto did not start (see $log)"
    exit 1
}

stop_server() {
    kill "$server" 2>/dev/null
    wait "$server" 2>/dev/null
}

check_sent() {
    local want=$1 tx part
    tx=$(printf '%s\n' "$want" | sed -n 's/.*pushed 4096, sha256 //p')
    part=$(printf '%s\n' "$want" | sed -n 's/.*tx partial 400, sha256 //p')
    if grep -q "WRITEBUF iio:device2 4096 bytes sha256 $tx" "$log"; then
        pass "the pretend transmitter got the samples sent"
    else
        fail "the pretend transmitter got the samples sent"
    fi
    if grep -q "WRITEBUF iio:device2 400 bytes sha256 $part" "$log"; then
        pass "and the partial push"
    else
        fail "and the partial push"
    fi
    if grep -q "OPEN iio:device2 256 00000003 CYCLIC" "$log"; then
        pass "a cyclic buffer opened as one"
    else
        fail "a cyclic buffer opened as one"
    fi
}

case "$mode" in
host)
    program=$2 address=$3 port=$4
    start_server "$port" "$address"
    got=$("$program" "ip:$address:$port" 2>&1)
    stop_server
    want=$(python3 -I tests/testiiod.py expect "$address")
    # (the Mac's own error numbers and texts where RitterOS's differ)
    got=$(printf '%s\n' "$got" | sed -e 's/errno 78$/errno 38/' \
                                     -e 's/: Resource busy$/: Device or resource busy/')
    compare "iiotest on this Mac: every line as the model's" "$want" "$got"
    check_sent "$want"
    ;;
ritteros)
    host=$2 from=$3 port=$4 arch=$5
    shift 5
    ssh=(ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new "$@" "$host")
    bind=127.0.0.1
    [ "$from" = 10.0.2.2 ] || bind=0.0.0.0      # (a real network: reachable from it)
    uri="ip:$from:$port"
    start_server "$port" "$bind"
    got=$("${ssh[@]}" "/system/$arch/test/iiotest $uri" 2>&1)
    info=$("${ssh[@]}" "iio_info -u $uri" 2>&1)
    wrote=$("${ssh[@]}" "iio_attr -u $uri -c ad9361-phy RX_LO frequency 433920000" 2>&1)
    quiet=$("${ssh[@]}" "iio_attr -u $uri -q -c -o ad9361-phy voltage0 hardwaregain" 2>&1)
    bogus=$("${ssh[@]}" "iio_attr -u $uri -c ad9361-phy RX_LO bogus" 2>&1; echo "exit $?")
    stop_server
    want=$(python3 -I tests/testiiod.py expect "$from")
    compare "iiotest on RitterOS: every line as the model's" "$want" "$got"
    check_sent "$want"
    for line in "IIO context created with network backend." \
                "Backend version: 0.26 (git tag: 0000000)" \
                "ip,ip-addr: $from" \
                "iio:device3: cf-ad9361-lpc (buffer capable)" \
                "voltage0: vccint (input, index: 1, format: le:s12/16>>4)" \
                "attr  3: fastlock_recall ERROR: Invalid argument" \
                "debug attr  0: direct_reg_access value: 0x5A" \
                "Current trigger: trigger0(test-trigger)"; do
        if printf '%s\n' "$info" | grep -qF -- "$line"; then
            pass "iio_info: $line"
        else
            fail "iio_info: $line"
        fi
    done
    compare "iio_attr writes and reads back" "$(printf '%s\n%s' "wrote 10 bytes to frequency" \
            "dev 'ad9361-phy', channel 'altvoltage0' (output), id 'RX_LO', attr 'frequency', value '433920000'")" \
            "$wrote"
    compare "iio_attr -q, an output channel (as iiotest left it)" "-20.250000" "$quiet"
    compare "iio_attr: an attribute the channel lacks" \
            "$(printf "iio_attr: 'RX_LO' has no attr 'bogus'\nexit 1")" "$bogus"
    ;;
*)
    sed -n '2,13p' "$0"
    exit 2
    ;;
esac

if [ $failures -ne 0 ]; then
    echo "$failures check(s) failed."
    exit 1
fi
echo "All checks passed."

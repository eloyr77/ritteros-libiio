# ritteros-libiio

libiio (the classic 0.26 interface, `iio.h`) for [RitterOS](https://github.com/eloyr77/RitterOS),
to program an ADALM-Pluto (or another IIO device behind `iiod`) from RitterOS
over the network. Our own code from iiod's text protocol, with libiio's tools
`iio_info` and `iio_attr`.

```sh
make                              # with RitterOS checked out at ../RitterOS
make install HOST=root@ADDRESS    # the tools onto a RitterOS machine, over ssh
iio_info -u ip:192.168.2.1        # on RitterOS: everything the Pluto has
iio_attr -u ip:192.168.2.1 -c ad9361-phy RX_LO frequency 433920000
```

See `CLAUDE.md` for building, checking and the facts the code depends on.

# N-Boot bootctrl tool

`bootctrl.py` creates and inspects the two redundant 4096-byte records consumed
by the `bootnuttx` command.

Create metadata for one NuttX slot:

```sh
python3 tools/nboot/bootctrl.py init \
  --output bootctrl.bin \
  --nuttx-a nuttx.bin
```

Create metadata with NuttX and AMP A/B payloads:

```sh
python3 tools/nboot/bootctrl.py init \
  --output bootctrl.bin \
  --nuttx-a nuttx-a.bin \
  --nuttx-b nuttx-b.bin \
  --amp-a amp-a.itb \
  --amp-b amp-b.itb
```

The A slot starts at priority 15. A supplied B slot starts at priority 14.
Missing images leave their slots disabled. Both records are initialized with
the same generation and CRC32.

Inspect an image:

```sh
python3 tools/nboot/bootctrl.py inspect bootctrl.bin
```

Run the focused tests:

```sh
python3 -m unittest tools/nboot/test_bootctrl.py
```

To enter `N-Boot>` when the full profile uses zero boot delay, run the serial
sender while resetting the board:

```sh
python3 tools/nboot/request_recovery.py --port /dev/ttyUSB0
```

The sender repeats the single-byte `!` stop token across the reset boundary.

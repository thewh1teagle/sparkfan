# sparkfan

Fan control for the NVIDIA DGX Spark (GB10).

Linux sees no fan on this machine. The EC owns the curve, and the only way to
ask for more air is the EC's own "raise the RPM floor" command, reachable
through a kernel-only FF-A call. sparkfan is that call, gated, plus a small
daemon that drives it from temperature.

```
sudo sparkfan set 9000     # fans never below 9000 rpm (valid 1260–13500)
sudo sparkfan max          # 13500 rpm, loud
sudo sparkfan auto         # back to the stock curve
sudo sparkfan status       # caps, floor, measured fan RPM, fault, temps
sudo sparkfan daemon       # stock curve until 75 °C, then 6000 → 9000 (80 °C) → 13500 (85 °C)
```

The number is a floor in rpm: the EC keeps running its own curve, but never
below that. Higher floor, more air, more noise. 9000 is a good working level,
that's fan0 at 100 % and fan1 at two thirds.

The daemon exists because the stock curve reacts too late on some units (54 %
fan at 90 °C, 75 % at 95 °C, then a hard thermal reset). It leaves the EC alone
below 75 °C and only steps in when the board creeps.

## Layout

| path | role |
|---|---|
| `kmod/` | C kernel gate: sysfs `floor` `caps` `telemetry` `fault` on the EC FF-A device |
| `cli/` | Rust CLI and daemon, std only |
| `sparkfan.service` | systemd unit for the daemon |
| `gpu-clock-cap.sh` | optional system-wide GPU clock cap, see docs |
| `docs/` | signing, protocol, safety |

## Build and install

```
make           # build/kmod/sparkfan.ko, build/cli/release/sparkfan
make test
```

Secure Boot is on by default on the Spark: sign the module once with your own key,
see [docs/secure-boot.md](docs/secure-boot.md). Then:

```
sudo mkdir -p /lib/modules/$(uname -r)/extra && sudo install -m 644 build/kmod/sparkfan.ko /lib/modules/$(uname -r)/extra/ && sudo depmod -a
sudo install -m 755 build/cli/release/sparkfan /usr/local/bin/
sudo modprobe sparkfan && sudo sparkfan set 9000
sudo install -m 644 sparkfan.service /etc/systemd/system/ && sudo systemctl enable --now sparkfan
```

## Safety

Floor only. The module never writes the cap slot, so it cannot cool less than
stock. Requests are clamped to the EC's reported fan range, serialized, echoed
back and verified, and any transport fault latches the device read-only.
Nothing is sent on load or unload. Details in [docs/safety.md](docs/safety.md).

## More

- [docs/protocol.md](docs/protocol.md): the EC mailbox, byte by byte
- [docs/secure-boot.md](docs/secure-boot.md): sign the module, keep Secure Boot on
- [docs/gpu-clock-cap.md](docs/gpu-clock-cap.md): optional clock cap for extra headroom (2400 MHz suggested)

Protocol reverse engineering by [841973620](https://github.com/Z841973620/dgx-spark-fan-override)
and [xXLegionBinFrogXx](https://github.com/xXLegionBinFrogXx/gb10-fan-control). GPL-2.0.

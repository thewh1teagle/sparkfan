# clockcap: optional GPU clock cap

The fan floor keeps a loaded DGX Spark around 80–86 °C. That is normal, but it
leaves little margin for a workload that does not pace itself. A clock cap is
cheap insurance that applies to every process, not just one trainer.

Under sustained load the GB10 settles near 2300–2400 MHz by itself, so capping
at **2400 MHz** costs almost nothing in throughput and removes the short bursts
to 3003 MHz that spike the board temperature.

```
sudo ./gpu-clock-cap.sh 2400     # now
sudo ./gpu-clock-cap.sh reset    # undo
```

Measured on one unit at full training load: 3003 MHz cap → board 93 °C in a
minute with stock fans; 2000 MHz → ~50 W, board 77–86 °C; 1500 MHz → ~30 W,
board 71–83 °C.

## Make it persistent

The cap is lost on reboot. To restore it at boot:

```
sudo install -m 755 gpu-clock-cap.sh /usr/local/bin/
sudo install -m 644 gpu-clock-cap.service /etc/systemd/system/
sudo systemctl enable --now gpu-clock-cap
```

Edit the MHz in `/etc/systemd/system/gpu-clock-cap.service` to taste. Power
limits (`nvidia-smi -pl`) are not supported on the GB10, the clock lock is the
only knob.

# EC fan protocol

Everything goes through one path:

```
sparkfan.ko → FF-A Direct Request 2 → EC eSPI partition (arm-ffa-N, uuid 884a63a0-3285-4120-83aa-eec008a0a546)
            → OEM command 17 + shared page ns_shm0 (phys 0x933dd000, 4 KiB)
            → EC outer service 0x07 (thermal mailbox) → inner command
```

## Shared page (command 17)

| offset | size | meaning |
|---:|---:|---|
| 0x00 | 1 | input length |
| 0x01 | 1 | output length |
| 0x02 | 1 | EC output start offset (0) |
| 0x03 | 1 | accepted |
| 0x04 | 1 | ready (1 when the reply is in) |
| 0x10 | n | request bytes, overwritten by the reply |

Service status in the FF-A reply word: 0 ok, 5 eSPI failure/timeout, 10 EC mailbox busy.

## Inner frame

`[0x07, cmd, status, data...]`, status 0 on request and on success, 0xff unsupported.

| cmd | dir | data |
|---:|---|---|
| 1 | read | caps: `07 01 00 cap mode f0min f0max f1min f1max` (LE16 each). Spark: mode 0 (RPM), fan0 1260–9000, fan1 1890–13500 |
| 3 | write | cap slot 0x119190, LE16 rpm. **Never used by sparkfan** |
| 5 | write | floor slot 0x119192, LE16 rpm, `0xffff` = disabled |
| 7 | read | 64-byte thermal/fan snapshot at reply+3. Observed: LE16 words 2 and 3 = measured fan0 / fan1 RPM (e.g. 2700/4050 idle, 9000/8910 with floor 9000); word 0 = 0x1f01 header. Exposed raw as `telemetry`, decoded by `sparkfan status` |

Examples, as seen on the wire:

```
full speed   07 05 00 BC 34      (13500)
stock curve  07 05 00 FF FF
```

The EC converts the floor to PWM per fan (fan0 pins at 100 % above 9000), ramps
+10 %/−1 % per control tick, and clamps at 100 %. The floor is volatile: any
reboot or power cycle returns the EC to its stock curve.

## Known quirk

Several GB10 units acknowledge overrides but keep the stock curve until one
full power cycle (shutdown, unplug, hold power 10 s, plug, boot). Do that once
before blaming software.

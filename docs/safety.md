# Safety model

What sparkfan can do to the machine: raise the fan RPM floor. Nothing else.

## Gates in the kernel module

- **Floor only.** Inner command 3 (cap) is not implemented. The EC can never be told to cool less than stock.
- **Clamped.** A requested floor is clamped into the EC-reported fan1 range (`caps`, fallback 1–13500). `max` resolves to fan1 max.
- **Serialized.** One EC exchange at a time (mutex).
- **Idle check.** No request is sent while the shared mailbox shows a pending exchange (`EBUSY`).
- **Echo check.** The reply must repeat outer, inner and rpm with status 0, else `EPROTO`.
- **Page restore.** The shared page is snapshotted before and restored after every exchange; a failed restore latches.
- **Fault latch.** FF-A transport failure, service status ≠ 0, or reply timeout latches `fault`; the device then refuses all requests until the module is reloaded, ideally after a power cycle. EC state after a fault is unknown by design.
- **Silent load/unload.** No EC traffic on probe or remove.
- **Reserved page check.** Refuses to map ns_shm0 if the kernel owns that page.

## Gates in the daemon

- Stock curve (floor auto) below the first curve point; default `75:6000,80:9000,85:13500` on the hottest of board zones / GPU.
- 5 °C hysteresis on the way down, immediate on the way up.
- Exits on a latched fault instead of retrying.

## What is not covered

- Tachometer readback comes from EC telemetry words 2/3 (`sparkfan status` → `rpm fan0 .. fan1 ..`), verified on one unit; `floor` itself is the last acknowledged request.
- Fan wear: a permanent high floor is more runtime. Choose the lowest floor that keeps temperatures where you want them.
- The EC's own protections (100 % clamp, thermal trips) stay in force underneath.

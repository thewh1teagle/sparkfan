//! sparkfan: userspace brain for the sparkfan kernel gate (DGX Spark / GB10).
//!
//!   sparkfan status                 caps, floor, fault, temps
//!   sparkfan set 9000               request a fan RPM floor
//!   sparkfan auto                   hand the floor back to the EC curve
//!   sparkfan max                    fan1 max (13500 on Spark)
//!   sparkfan daemon [curve]         stock curve until the board creeps, then hold a floor
//!        curve = "75:6000,80:9000,85:13500"  (default), auto below the first point,
//!        temperature is EMA-smoothed (tau 20 s); up instantly on the smoothed value,
//!        down only after 60 s below (threshold - 5 C); poll 5 s
//!
//! Build: rustc -O main.rs -o sparkfan   (std only). Needs root for set/auto/max/daemon.

use std::{env, fs, path::PathBuf, process, thread, time::Duration};

const FFA_BUS: &str = "/sys/bus/arm_ffa/devices";
const THERMAL: &str = "/sys/class/thermal";
const DEFAULT_CURVE: &str = "75:6000,80:9000,85:13500";
const HYST_C: f64 = 5.0;
const POLL_S: u64 = 5;
const HOLD_DOWN_S: u64 = 60; // a lower floor is only applied after the temperature stayed low this long
const EMA_TAU_S: f64 = 20.0; // sensor smoothing time constant; bursty loads swing the raw reading 10+ C

fn die(msg: &str) -> ! {
    eprintln!("sparkfan: {msg}");
    process::exit(1)
}

/// Directory of the sparkfan sysfs group (created by the kernel module on the EC eSPI partition).
fn gate() -> PathBuf {
    let Ok(rd) = fs::read_dir(FFA_BUS) else { die("no FF-A bus; not a GB10?") };
    for e in rd.flatten() {
        let p = e.path().join("sparkfan");
        if p.is_dir() {
            return p;
        }
    }
    die("sparkfan gate not found: modprobe sparkfan (see README for Secure Boot signing)")
}

fn read(p: &PathBuf, name: &str) -> String {
    fs::read_to_string(p.join(name)).map(|s| s.trim().to_string()).unwrap_or_else(|e| format!("error {e}"))
}

fn write_floor(p: &PathBuf, v: &str) {
    if let Err(e) = fs::write(p.join("floor"), v) {
        die(&format!("write floor={v}: {e} (root? fault latched? check `sparkfan status`)"));
    }
}

/// Hottest thermal zone (board) in C, plus GPU temp from nvidia-smi if available.
fn temps() -> (f64, Option<f64>) {
    let mut board: f64 = -1.0;
    if let Ok(rd) = fs::read_dir(THERMAL) {
        for e in rd.flatten() {
            if !e.file_name().to_string_lossy().starts_with("thermal_zone") {
                continue;
            }
            if let Ok(s) = fs::read_to_string(e.path().join("temp")) {
                if let Ok(mc) = s.trim().parse::<f64>() {
                    board = board.max(mc / 1000.0);
                }
            }
        }
    }
    let gpu = process::Command::new("nvidia-smi")
        .args(["--query-gpu=temperature.gpu", "--format=csv,noheader"])
        .output()
        .ok()
        .and_then(|o| String::from_utf8(o.stdout).ok())
        .and_then(|s| s.trim().parse::<f64>().ok());
    (board, gpu)
}

/// EC telemetry (inner cmd 7): words 2 and 3 track the two fans' measured RPM (fan0, fan1).
fn parse_rpm(hex: &str) -> Option<(u32, u32)> {
    let b: Vec<u8> = (0..hex.len().saturating_sub(1)).step_by(2).filter_map(|i| u8::from_str_radix(&hex[i..i + 2], 16).ok()).collect();
    if b.len() < 8 {
        return None;
    }
    let w = |i: usize| u32::from(b[i]) | (u32::from(b[i + 1]) << 8);
    Some((w(4), w(6)))
}

fn parse_curve(s: &str) -> Vec<(f64, u32)> {
    let mut v: Vec<(f64, u32)> = s
        .split(',')
        .map(|pair| {
            let (t, r) = pair.split_once(':').unwrap_or_else(|| die(&format!("bad curve point {pair:?}, want temp:rpm")));
            (t.trim().parse().unwrap_or_else(|_| die("bad temp")), r.trim().parse().unwrap_or_else(|_| die("bad rpm")))
        })
        .collect();
    v.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap());
    v
}

/// Exponential moving average with time constant `tau` seconds, sampled every `dt` seconds.
fn ema(prev: Option<f64>, x: f64, dt: f64, tau: f64) -> f64 {
    match prev {
        None => x,
        Some(p) => {
            let a = 1.0 - (-dt / tau).exp();
            p + a * (x - p)
        }
    }
}

/// Floor for a temperature: highest curve point whose threshold is reached; 0 = auto.
fn floor_for(curve: &[(f64, u32)], t: f64) -> u32 {
    curve.iter().filter(|(th, _)| t >= *th).map(|(_, r)| *r).last().unwrap_or(0)
}

fn daemon(curve: Vec<(f64, u32)>) {
    let g = gate();
    let mut current: u32 = u32::MAX; // unknown
    let mut low_since: Option<std::time::Instant> = None; // when the temperature first allowed a step down
    let mut smooth: Option<f64> = None;
    eprintln!("sparkfan daemon: curve {curve:?}, ema {EMA_TAU_S} s, hysteresis {HYST_C} C, hold-down {HOLD_DOWN_S} s, poll {POLL_S} s, gate {}", g.display());
    loop {
        let (board, gpu) = temps();
        let raw = board.max(gpu.unwrap_or(-1.0));
        let t = ema(smooth, raw, POLL_S as f64, EMA_TAU_S);
        smooth = Some(t);
        let mut want = floor_for(&curve, t);
        if current != u32::MAX && want < current {
            // step down only when HYST_C below the threshold that earned the current floor,
            // and only after that has held for HOLD_DOWN_S (load gaps cool the board for seconds at a time)
            let th = curve.iter().find(|(_, r)| *r == current).map(|(th, _)| *th).unwrap_or(f64::MAX);
            if t > th - HYST_C {
                low_since = None;
                want = current;
            } else {
                let since = *low_since.get_or_insert_with(std::time::Instant::now);
                if since.elapsed().as_secs() < HOLD_DOWN_S {
                    want = current;
                }
            }
        } else {
            low_since = None;
        }
        if want != current {
            let v = if want == 0 { "auto".to_string() } else { want.to_string() };
            write_floor(&g, &v);
            eprintln!("sparkfan: smoothed {t:.0} C (board {board:.0}, gpu {}) -> floor {v} (was {})", gpu.map(|g| format!("{g:.0}")).unwrap_or("n/a".into()), if current == u32::MAX { "unknown".into() } else { current.to_string() });
            current = want;
            low_since = None;
        }
        let fault = read(&g, "fault");
        if fault != "0" {
            eprintln!("sparkfan: gate fault {fault}; EC state unknown, reload module after a power cycle");
            process::exit(2);
        }
        thread::sleep(Duration::from_secs(POLL_S));
    }
}

fn main() {
    let args: Vec<String> = env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("status") => {
            let g = gate();
            let (board, gpu) = temps();
            println!("gate      {}", g.display());
            println!("caps      {}", read(&g, "caps"));
            println!("floor     {}", read(&g, "floor"));
            println!("fault     {}", read(&g, "fault"));
            let tele = read(&g, "telemetry");
            match parse_rpm(&tele) {
                Some((f0, f1)) => println!("rpm       fan0 {f0}  fan1 {f1}"),
                None => println!("telemetry {tele}"),
            }
            println!("board     {board:.0} C   gpu {}", gpu.map(|g| format!("{g:.0} C")).unwrap_or("n/a".into()));
        }
        Some("set") => {
            let rpm: u32 = args.get(1).and_then(|s| s.parse().ok()).unwrap_or_else(|| die("usage: sparkfan set <rpm>"));
            write_floor(&gate(), &rpm.to_string());
            println!("floor {}", read(&gate(), "floor"));
        }
        Some("auto") => { write_floor(&gate(), "auto"); println!("floor auto"); }
        Some("max") => { write_floor(&gate(), "max"); println!("floor {}", read(&gate(), "floor")); }
        Some("daemon") => daemon(parse_curve(args.get(1).map(String::as_str).unwrap_or(DEFAULT_CURVE))),
        _ => die("usage: sparkfan status | set <rpm> | auto | max | daemon [t:rpm,...]"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rpm_from_telemetry_words() {
        // observed: idle "011f00008c0ad20f..." -> 2700/4050 ; floor 9000 -> 9000/8910
        assert_eq!(parse_rpm("011f00008c0ad20f0000"), Some((2700, 4050)));
        assert_eq!(parse_rpm("011f00002823ce220000"), Some((9000, 8910)));
        assert_eq!(parse_rpm("error 5"), None);
    }

    #[test]
    fn ema_smooths_bursts() {
        let mut s = None;
        for x in [80.0, 70.0, 80.0, 70.0, 80.0, 70.0] {
            s = Some(ema(s, x, 5.0, 20.0));
        }
        let v = s.unwrap();
        assert!(v > 73.0 && v < 78.0, "{v}"); // a 10 C square wave settles near its mean
        assert_eq!(ema(None, 42.0, 5.0, 20.0), 42.0); // first sample passes through
    }

    #[test]
    fn curve_parses_and_sorts() {
        let c = parse_curve("80:9000, 60:3000,70:6000");
        assert_eq!(c, vec![(60.0, 3000), (70.0, 6000), (80.0, 9000)]);
    }

    #[test]
    fn floor_picks_highest_reached_threshold() {
        let c = parse_curve(DEFAULT_CURVE);
        assert_eq!(floor_for(&c, 70.0), 0); // stock curve until the first point
        assert_eq!(floor_for(&c, 75.0), 6000);
        assert_eq!(floor_for(&c, 79.9), 6000);
        assert_eq!(floor_for(&c, 80.0), 9000);
        assert_eq!(floor_for(&c, 120.0), 13500);
    }

    #[test]
    fn hysteresis_holds_floor_until_cooled() {
        // mirrors the daemon rule: step down only when t <= threshold - HYST_C
        let c = parse_curve(DEFAULT_CURVE);
        let current = 9000u32; // earned at 80 C
        let th = c.iter().find(|(_, r)| *r == current).map(|(t, _)| *t).unwrap();
        let decide = |t: f64| { let w = floor_for(&c, t); if w < current && t > th - HYST_C { current } else { w } };
        assert_eq!(decide(78.0), 9000); // within hysteresis band
        assert_eq!(decide(75.0), 6000); // cooled enough
        assert_eq!(decide(84.0), 9000);
        assert_eq!(decide(85.0), 13500); // stepping up is immediate
    }
}

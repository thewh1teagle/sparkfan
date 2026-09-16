# Signing the module with Secure Boot on

Ubuntu on the Spark boots with Secure Boot enabled and rejects unsigned modules
(`insmod: Key was rejected by service`). Enroll a Machine Owner Key once, then
sign every build with it. Secure Boot stays on.

## 1. Create and enroll a key (once, needs a keyboard at the next boot)

```
sudo apt install mokutil openssl
openssl req -new -x509 -newkey rsa:2048 -nodes -days 36500 \
        -subj "/CN=sparkfan MOK/" -keyout MOK.priv -outform DER -out MOK.der
sudo mokutil --import MOK.der        # pick a one-time password
sudo reboot
```

At boot a blue "MOK management" screen appears: Enroll MOK → Continue → Yes →
type the password → reboot. Verify:

```
mokutil --list-enrolled | grep sparkfan
```

Keep `MOK.priv` private (`chmod 600`, outside git).

## 2. Sign each build

```
sudo /lib/modules/$(uname -r)/build/scripts/sign-file sha512 MOK.priv MOK.der build/kmod/sparkfan.ko
modinfo build/kmod/sparkfan.ko | grep sig_key
```

Repeat after every kernel update (the module must be rebuilt for the new kernel).

## Alternative

Disable Secure Boot in UEFI setup (Del/F2 at boot → Secure Boot → Disabled).
Unsigned modules then load. Also needs a keyboard once.

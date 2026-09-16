# make        -> build/kmod/sparkfan.ko + build/cli/release/sparkfan
# make test   -> C protocol tests + Rust tests
all:   ; $(MAKE) -C kmod && cargo build --release --manifest-path cli/Cargo.toml --target-dir build/cli
test:  ; $(MAKE) -C kmod test && cargo test --manifest-path cli/Cargo.toml --target-dir build/cli
clean: ; rm -rf build

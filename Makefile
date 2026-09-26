CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic

.PHONY: all clean install

all: surface-ir-bridge

surface-ir-bridge: src/surface_ir_bridge.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f surface-ir-bridge

install: surface-ir-bridge
	install -Dm755 surface-ir-bridge /usr/local/bin/surface-ir-bridge
	install -Dm755 scripts/setup_ipu3.sh /usr/local/bin/setup_ipu3.sh
	install -Dm755 scripts/capture_ir_pair.sh /usr/local/bin/capture_ir_pair.sh
	install -Dm644 systemd/surface-ir-camera.service /etc/systemd/system/surface-ir-camera.service
	install -Dm644 modprobe.d/surface-ir.conf /etc/modprobe.d/surface-ir.conf

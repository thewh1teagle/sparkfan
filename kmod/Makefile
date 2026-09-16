# out-of-tree module; artifacts go to ../build/kmod (Kbuild runs there on symlinked sources)
KDIR := /lib/modules/$(shell uname -r)/build
OUT  := $(abspath ../build/kmod)
all:
	mkdir -p $(OUT) && ln -sf $(CURDIR)/sparkfan.c $(CURDIR)/sparkfan_proto.h $(OUT)/ && echo 'obj-m += sparkfan.o' > $(OUT)/Kbuild
	$(MAKE) -C $(KDIR) M=$(OUT) modules
test:
	mkdir -p $(OUT) && cc -Wall -Wextra -Werror test.c -o $(OUT)/test && $(OUT)/test
clean: ; rm -rf $(OUT)

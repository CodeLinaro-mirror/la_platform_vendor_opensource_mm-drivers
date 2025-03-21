# SPDX-License-Identifier: GPL-2.0-only

MM_ROOT=$(ROOTDIR)display/vendor/qcom/opensource/mm-drivers
KBUILD_OPTIONS := MM_ROOT=$(MM_ROOT)

obj-m += msm_ext_display/

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) INSTALL_MOD_STRIP=1 -C $(KERNEL_SRC) M=$(M) modules_install

%:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@ $(KBUILD_OPTIONS)

clean:
	rm -f *.o *.ko *.mod.c *.mod.o *~ .*.cmd Module.symvers
	rm -rf .tmp_versions

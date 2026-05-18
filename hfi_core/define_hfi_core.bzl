load("//build/kernel/kleaf:kernel.bzl", "ddk_module")
load("//vendor/qcom/opensource/mm-drivers:target_variants.bzl", "get_all_variants", "targets", "get_16k_tv", "target_16k")
load("@rules_pkg//pkg:install.bzl", "pkg_install")
load("@rules_pkg//pkg:mappings.bzl", "pkg_files", "strip_prefix")

def _define_module(target, variant):
    tv = "{}_{}".format(target, variant)

    deps = select({
        "//build/qcom_build_extensions:qtisocrepo_true": [
            "//soc-repo:all_headers",
            "//soc-repo:{}/drivers/firmware/qcom/qcom-scm".format(tv),
            "//soc-repo:{}/drivers/soc/qcom/mdt_loader".format(tv),
            "//soc-repo:{}/drivers/soc/qcom/smem".format(tv),
        ],
        "//build/qcom_build_extensions:qtisocrepo_false": [
            "//msm-kernel:all_headers",
        ],
    })

    kernel_build = select({
        "//build/qcom_build_extensions:qtisocrepo_true": "//soc-repo:{}_base_kernel".format(tv),
        "//build/qcom_build_extensions:qtisocrepo_false": "//msm-kernel:{}".format(tv),
    })

    # some targets do not have synx available, accordingly disable hw-fence and avoid dependency
    if target in ["canoe-tuivm", "canoe-oemvm"]:
        target_config = "canoevm_defconfig"
    elif target in ["art-tuivm", "art-oevm"]:
        target_config = "artoevm_defconfig"
    else:
        target_config = "defconfig"

    ddk_module(
        name = "{}_msm_hfi_core".format(tv),
        srcs = [
            "src/hfi_transport/hfi_queue.c",
            "src/hfi_transport/hfi_ipc.c",
            "src/hfi_transport/hfi_smmu.c",
            "src/hfi_transport/hfi_swi.c",
            "src/hfi_transport/hfi_queue_controller.c",
            "src/hfi_transport/hfi_if_abstraction.c",
            "src/hfi_base/hfi_core.c",
            "src/hfi_base/hfi_core_irq.c",
            "src/hfi_base/hfi_core_firmware.c",
            "src/hfi_base/hfi_core_ssr.c",
            "src/hfi_dbg_packet.c",
            "src/hfi_core_debug.c",
            "src/hfi_core_probe.c",
        ],
        out = "msm_hfi_core.ko",
        defconfig = target_config,
        kconfig = "Kconfig",
        deps = deps + [
            "//vendor/qcom/opensource/mm-drivers:mm_drivers_headers",
        ],
        kernel_build = kernel_build,
    )

    pkg_files(
        name = tv + "_dist_files",
        srcs = [":{}_msm_hfi_core".format(tv)],
        visibility = ["//visibility:private"],
        strip_prefix = strip_prefix.files_only(),
    )

    pkg_install(
        name = "{}_msm_hfi_core_dist".format(tv),
        srcs = [":{}_dist_files".format(tv)],
        destdir = "out/target/product/{}/dlkm/lib/modules".format(target),
    )

def matching_la_variant(target_16k):
    for target in targets:
        if target_16k.startswith(target):
            return target
    return None

def define_16k_aliases(t):
    target = "{}".format(matching_la_variant(t))
    native.alias(
        name = "{}_defconfig".format(t),
        actual = "{}_defconfig".format(target),
    )

def define_hfi_core():
    for target in target_16k:
        define_16k_aliases(target)
    for (t, v) in get_all_variants():
        if t == "parrot" or t == "malabar" or t == "bengal-le":
            continue
        _define_module(t, v)

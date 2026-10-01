load("//build/kernel/kleaf:kernel.bzl", "ddk_module")
load("@rules_pkg//pkg:install.bzl", "pkg_install")
load("@rules_pkg//pkg:mappings.bzl", "pkg_files", "strip_prefix")
load("//vendor/qcom/opensource/mm-drivers:target_variants.bzl", "get_all_variants", "targets")

def _define_module(target, variant):
    tv = "{}_{}".format(target, variant)

    deps = select({
        "//build/qcom_build_extensions:qtisocrepo_true": [
            "//soc-repo:all_headers",
            "//soc-repo:{}/drivers/remoteproc/rproc_qcom_common".format(tv),
            "//soc-repo:{}/drivers/remoteproc/qcom_q6v5_pas".format(tv),
            "//soc-repo:{}/drivers/virt/gunyah/gh_dbl".format(tv),
            "//soc-repo:{}/drivers/virt/gunyah/gh_rm_drv".format(tv),
            "//soc-repo:{}/drivers/firmware/qcom/qcom-scm".format(tv),
            "//soc-repo:{}/drivers/soc/qcom/hab/msm_hab".format(tv),
        ],
        "//build/qcom_build_extensions:qtisocrepo_false": [
            "//msm-kernel:all_headers",
        ],
    })
    kernel_build = select({
        "//build/qcom_build_extensions:qtisocrepo_true": "//soc-repo:{}_base_kernel".format(tv),
        "//build/qcom_build_extensions:qtisocrepo_false": "//msm-kernel:{}".format(tv),
    })

    if target in ["pineapple", "alor-le", "kalama-le", "pebble-le"]:
        target_config = "defconfig"
    else:
        target_config = "{}_defconfig".format(target)

    ddk_module(
        name = "{}_msm_hw_fence".format(tv),
        srcs = [
            "src/hw_fence_drv_debug.c",
            "src/hw_fence_drv_ipc.c",
            "src/hw_fence_drv_priv.c",
            "src/hw_fence_drv_utils.c",
            "src/hw_fence_ioctl.c",
            "src/msm_hw_fence.c",
        ],
        out = "msm_hw_fence.ko",
        defconfig = target_config,
        kconfig = "Kconfig",
        conditional_srcs = {
            "CONFIG_DEBUG_FS": {
                True: ["src/hw_fence_ioctl.c"],
            },
        },
        deps = deps + [
            "//vendor/qcom/opensource/mm-drivers:mm_drivers_headers",
        ],
        kernel_build = kernel_build,
    )

    pkg_files(
        name = tv + "_dist_files",
        srcs = [":{}_msm_hw_fence".format(tv)],
        visibility = ["//visibility:private"],
        strip_prefix = strip_prefix.files_only(),
    )

    pkg_install(
        name = "{}_msm_hw_fence_dist".format(tv),
        srcs = [":{}_dist_files".format(tv)],
        destdir = "out/target/product/{}/dlkm/lib/modules".format(target),
    )

def matching_la_variant(t):
    for target in targets:
        if t.startswith(target):
            return target
    return None

def define_hw_fence():
    for (t, v) in get_all_variants():
        if t == "parrot" or t == "malabar" or t == "bengal-le" or t == "shikra":
            continue
        _define_module(t, v)

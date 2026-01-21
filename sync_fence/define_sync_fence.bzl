load("//build/kernel/kleaf:kernel.bzl", "ddk_module")
load("//vendor/qcom/opensource/mm-drivers:target_variants.bzl", "get_all_variants")
load("@rules_pkg//pkg:install.bzl", "pkg_install")
load("@rules_pkg//pkg:mappings.bzl", "pkg_files", "strip_prefix")

def _define_module(target, variant):
    tv = "{}_{}".format(target, variant)

    deps = select({
        "//build/qcom_build_extensions:qtisocrepo_true": ["//soc-repo:all_headers"],
        "//build/qcom_build_extensions:qtisocrepo_false": ["//msm-kernel:all_headers"],
    })
    kernel_build = select({
        "//build/qcom_build_extensions:qtisocrepo_true": "//soc-repo:{}_base_kernel".format(tv),
        "//build/qcom_build_extensions:qtisocrepo_false": "//msm-kernel:{}".format(tv),
    })

    ddk_module(
        name = "{}_sync_fence".format(tv),
        srcs = ["src/qcom_sync_file.c"],
        out = "sync_fence.ko",
        kconfig = "Kconfig",
        defconfig = "defconfig",
        deps = deps + [
            "//vendor/qcom/opensource/mm-drivers:mm_drivers_headers",
        ],
        kernel_build = kernel_build,
    )

    pkg_files(
        name = tv + "_dist_files",
        srcs = [":{}_sync_fence".format(tv)],
        visibility = ["//visibility:private"],
        strip_prefix = strip_prefix.files_only(),
    )

    pkg_install(
        name = "{}_sync_fence_dist".format(tv),
        srcs = [":{}_dist_files".format(tv)],
        destdir = "out/target/product/{}/dlkm/lib/modules".format(target),
    )

def define_sync_fence():
    for (t, v) in get_all_variants():
        _define_module(t, v)

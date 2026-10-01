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


    # Module intentionally disabled for kernel-6.18.
    # Keep function structure so callers remain valid, but do not emit build targets.
    return

def define_sync_fence():
    for (t, v) in get_all_variants():
        _define_module(t, v)

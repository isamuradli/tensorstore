load("//bazel:tensorstore.bzl", "tensorstore_cc_binary")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])

tensorstore_cc_binary(
    name = "node1_writer",
    srcs = ["node1_writer.cc"],
    deps = [
        "//tensorstore",
        "//tensorstore:all_drivers",
        "//tensorstore:array",
        "//tensorstore:data_type",
        "//tensorstore:open",
        "//tensorstore:open_mode",
        "//tensorstore:spec",
        "//tensorstore/kvstore",
        "//tensorstore/kvstore/remote_dram",
        "//tensorstore/util:result",
        "//tensorstore/util:status",
        "@abseil-cpp//absl/strings:cord",
    ],
)

tensorstore_cc_binary(
    name = "node2_server",
    srcs = ["node2_server.cc"],
    deps = [
        "//tensorstore/kvstore",
        "//tensorstore/kvstore/remote_dram",
        "//tensorstore/util:result",
        "//tensorstore/util:status",
    ],
) 
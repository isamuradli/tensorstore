load("//bazel:tensorstore.bzl", "tensorstore_cc_binary")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])

cc_library(
    name = "tensor_transfer_extensions",
    srcs = ["tensor_transfer_extensions.cc"],
    hdrs = ["tensor_transfer_extensions.h"],
    deps = [
        "//tensorstore",
        "//tensorstore:array",
        "//tensorstore:data_type",
        "//tensorstore/kvstore",
        "//tensorstore/kvstore:operations",
        "//tensorstore/util:future",
        "//tensorstore/util:result",
        "//tensorstore/util:str_cat",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:cord",
        "@abseil-cpp//absl/strings:str_format",
    ],
)

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

tensorstore_cc_binary(
    name = "multi_writer_client",
    srcs = ["multi_writer_client.cc"],
    deps = [
        "//tensorstore",
        "//tensorstore:context",
        "//tensorstore/kvstore",
        "//tensorstore/kvstore/remote_dram",
        "//tensorstore/util:result",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:initialize",
        "@abseil-cpp//absl/strings:cord",
    ],
)

tensorstore_cc_binary(
    name = "zarr_tensor_client",
    srcs = ["zarr_tensor_client.cc"],
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
    name = "zarr_tensor_server",
    srcs = ["zarr_tensor_server.cc"],
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
    ],
)

tensorstore_cc_binary(
    name = "optimized_tensor_client",
    srcs = ["optimized_tensor_client.cc"],
    deps = [
        ":tensor_transfer_extensions",
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
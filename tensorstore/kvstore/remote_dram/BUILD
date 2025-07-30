# Remote DRAM KeyValueStore driver

load("//bazel:tensorstore.bzl", "tensorstore_cc_library", "tensorstore_cc_test", "tensorstore_cc_binary")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])

tensorstore_cc_library(
    name = "remote_dram",
    srcs = ["remote_dram_kvstore.cc"],
    hdrs = ["remote_dram_kvstore.h"],
    deps = [
        "//tensorstore/internal:intrusive_ptr",
        "//tensorstore/internal:uri_utils",
        "//tensorstore/internal/json_binding",
        "//tensorstore/kvstore",
        "//tensorstore/kvstore:byte_range",
        "//tensorstore/kvstore:generation",
        "//tensorstore/kvstore:key_range",
        "//tensorstore/util:executor",
        "//tensorstore/util:future",
        "//tensorstore/util:result",
        "//tensorstore/util:status",
        "//tensorstore/util:str_cat",
        "//tensorstore/util/execution",
        "//tensorstore/util/execution:future_sender",
        "//tensorstore/util/garbage_collection",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings:cord",
        "@abseil-cpp//absl/log:absl_log",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/synchronization",
        "@abseil-cpp//absl/time",
        "@ucx//:ucx",
    ],
    alwayslink = 1,
)

tensorstore_cc_binary(
    name = "verify_driver",
    srcs = ["verify_driver.cc"],
    deps = [
        ":remote_dram",
        "//tensorstore/kvstore",
        "@abseil-cpp//absl/status",
        "@nlohmann_json//:json",
    ],
)

tensorstore_cc_binary(
    name = "test_server_mode",
    srcs = ["test_server_mode.cc"],
    deps = [
        ":remote_dram",
        "//tensorstore/kvstore",
        "@abseil-cpp//absl/status",
        "@nlohmann_json//:json",
    ],
)

tensorstore_cc_binary(
    name = "test_write_operation",
    srcs = ["test_write_operation.cc"],
    deps = [
        ":remote_dram",
        "//tensorstore/kvstore",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings:cord",
        "@nlohmann_json//:json",
    ],
)

# Two-node test binaries for distributed testing
tensorstore_cc_binary(
    name = "two_node_test_server",
    srcs = ["two_node_test_server.cc"],
    deps = [
        ":remote_dram",
        "//tensorstore/kvstore",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings:cord",
        "@nlohmann_json//:json",
    ],
)

tensorstore_cc_binary(
    name = "two_node_test_client",
    srcs = ["two_node_test_client.cc"],
    deps = [
        ":remote_dram",
        "//tensorstore",
        "//tensorstore:array",
        "//tensorstore:context",
        "//tensorstore:data_type",
        "//tensorstore:open",
        "//tensorstore/index_space:dim_expression",
        "//tensorstore/kvstore",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings:cord",
        "@nlohmann_json//:json",
    ],
) 
load("//bazel:tensorstore.bzl", "tensorstore_cc_binary")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])

tensorstore_cc_binary(
    name = "compute_percentiles",
    srcs = [
        "compute_percentiles.cc",
        "data_type_invoker.h",
    ],
    deps = [
        "//tensorstore",
        "//tensorstore:all_drivers",
        "//tensorstore:array",
        "//tensorstore:context",
        "//tensorstore:contiguous_layout",
        "//tensorstore:data_type",
        "//tensorstore:index",
        "//tensorstore:open",
        "//tensorstore:open_mode",
        "//tensorstore:progress",
        "//tensorstore:rank",
        "//tensorstore:spec",
        "//tensorstore/index_space:dim_expression",
        "//tensorstore/index_space:index_transform",
        "//tensorstore/index_space:transformed_array",
        "//tensorstore/util:future",
        "//tensorstore/util:iterate_over_index_range",
        "//tensorstore/util:json_absl_flag",
        "//tensorstore/util:result",
        "//tensorstore/util:span",
        "//tensorstore/util:status",
        "//tensorstore/util:str_cat",
        "//tensorstore/util:utf8_string",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:marshalling",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
        "@net_sourceforge_half//:half",
        "@nlohmann_json//:json",
    ],
)

tensorstore_cc_binary(
    name = "create_array",
    srcs = ["create_array.cc"],
    deps = [
        "//tensorstore:array",
        "//tensorstore:index",
        "//tensorstore/index_space:dim_expression",
        "//tensorstore/index_space:index_transform",
        "//tensorstore/index_space:transformed_array",
        "//tensorstore/util:iterate_over_index_range",
        "//tensorstore/util:status",
    ],
)

tensorstore_cc_binary(
    name = "image_convolution",
    srcs = ["image_convolution.cc"],
    deps = [
        "//tensorstore:array",
        "//tensorstore:box",
        "//tensorstore:contiguous_layout",
        "//tensorstore:data_type",
        "//tensorstore:index",
        "//tensorstore/util:iterate_over_index_range",
        "//tensorstore/util:span",
        "@abseil-cpp//absl/functional:function_ref",
    ],
)

tensorstore_cc_binary(
    name = "map_apply",
    srcs = ["map_apply.cc"],
    deps = [
        "//tensorstore:array",
        "//tensorstore:index",
        "//tensorstore/index_space:dim_expression",
        "//tensorstore/index_space:transformed_array",
        "//tensorstore/util:iterate_over_index_range",
        "//tensorstore/util:status",
    ],
)

tensorstore_cc_binary(
    name = "extract_slice",
    srcs = ["extract_slice.cc"],
    deps = [
        "//tensorstore",
        "//tensorstore:all_drivers",
        "//tensorstore:array",
        "//tensorstore:context",
        "//tensorstore:data_type",
        "//tensorstore:index",
        "//tensorstore:open",
        "//tensorstore:open_mode",
        "//tensorstore:spec",
        "//tensorstore:strided_layout",
        "//tensorstore/index_space:dim_expression",
        "//tensorstore/index_space:index_transform",
        "//tensorstore/internal/image",
        "//tensorstore/internal/image:avif",
        "//tensorstore/internal/image:jpeg",
        "//tensorstore/internal/image:png",
        "//tensorstore/internal/image:webp",
        "//tensorstore/util:json_absl_flag",
        "//tensorstore/util:result",
        "//tensorstore/util:span",
        "//tensorstore/util:status",
        "//tensorstore/util:str_cat",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
        "@nlohmann_json//:json",
        "@riegeli//riegeli/bytes:fd_writer",
        "@riegeli//riegeli/bytes:std_io",
        "@riegeli//riegeli/bytes:writer",
    ],
)

tensorstore_cc_binary(
    name = "remote_dram_example",
    srcs = ["remote_dram_example.cc"],
    deps = [
        "//tensorstore",
        "//tensorstore:all_drivers",
        "//tensorstore:array",
        "//tensorstore:context",
        "//tensorstore:data_type",
        "//tensorstore:index",
        "//tensorstore:open",
        "//tensorstore:spec",
        "//tensorstore/kvstore",
        "//tensorstore/kvstore/remote_dram:remote_dram",
        "//tensorstore/util:result",
        "//tensorstore/util:status",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings:str_format",
        "@nlohmann_json//:json",
    ],
)

tensorstore_cc_binary(
    name = "remote_dram_server",
    srcs = ["remote_dram_server.cc"],
    deps = [
        "//tensorstore/kvstore",
        "//tensorstore/kvstore/remote_dram:remote_dram",
        "//tensorstore/util:result",
        "//tensorstore/util:status",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings:str_format",
        "@nlohmann_json//:json",
    ],
)

tensorstore_cc_binary(
    name = "remote_dram_client",
    srcs = ["remote_dram_client.cc"],
    deps = [
        "//tensorstore",
        "//tensorstore:all_drivers",
        "//tensorstore:array",
        "//tensorstore:context",
        "//tensorstore:data_type",
        "//tensorstore:index",
        "//tensorstore:open",
        "//tensorstore:open_mode",
        "//tensorstore:spec",
        "//tensorstore/kvstore/remote_dram:remote_dram",
        "//tensorstore/util:result",
        "//tensorstore/util:status",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings:str_format",
        "@nlohmann_json//:json",
    ],
)


tensorstore_cc_binary(
    name = "simple_remote_dram_server",
    srcs = ["simple_remote_dram_server.cc"],
    deps = [
        "//tensorstore/kvstore",
        "//tensorstore/kvstore/remote_dram",
        "//tensorstore/util:result",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:initialize",
    ],
)

tensorstore_cc_binary(
    name = "simple_remote_dram_tensor_client",
    srcs = ["simple_remote_dram_tensor_client.cc"],
    deps = [
        "//tensorstore",
        "//tensorstore:all_drivers",
        "//tensorstore:array",
        "//tensorstore:context",
        "//tensorstore:data_type",
        "//tensorstore:index",
        "//tensorstore:open",
        "//tensorstore/index_space:dim_expression",
        "//tensorstore/kvstore/remote_dram",
        "//tensorstore/util:result",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:initialize",
    ],
)

tensorstore_cc_binary(
    name = "writer_client",
    srcs = ["writer_client.cc"],
    deps = [
        "//tensorstore",
        "//tensorstore:all_drivers",
        "//tensorstore:array",
        "//tensorstore:context",
        "//tensorstore:data_type",
        "//tensorstore:index",
        "//tensorstore:open",
        "//tensorstore/index_space:dim_expression",
        "//tensorstore/kvstore/remote_dram",
        "//tensorstore/util:result",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:initialize",
    ],
)

tensorstore_cc_binary(
    name = "reader_client",
    srcs = ["reader_client.cc"],
    deps = [
        "//tensorstore",
        "//tensorstore:all_drivers",
        "//tensorstore:array",
        "//tensorstore:context",
        "//tensorstore:data_type",
        "//tensorstore:index",
        "//tensorstore:open",
        "//tensorstore/index_space:dim_expression",
        "//tensorstore/kvstore/remote_dram",
        "//tensorstore/util:result",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:initialize",
    ],
)

tensorstore_cc_binary(
    name = "simple_writer_client",
    srcs = ["simple_writer_client.cc"],
    deps = [
        "//tensorstore/kvstore",
        "//tensorstore/kvstore/remote_dram",
        "//tensorstore/util:result",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:initialize",
    ],
)

tensorstore_cc_binary(
    name = "simple_reader_client",
    srcs = ["simple_reader_client.cc"],
    deps = [
        "//tensorstore/kvstore",
        "//tensorstore/kvstore/remote_dram",
        "//tensorstore/util:result",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:initialize",
    ],
)

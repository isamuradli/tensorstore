# Copyright 2025 The TensorStore Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# buildifier: disable=module-docstring

load("@bazel_tools//tools/build_defs/repo:utils.bzl", "maybe")
load("//third_party:repo.bzl", "third_party_http_archive")

def repo():
    maybe(
        third_party_http_archive,
        name = "ucx",
        doc_name = "UCX",
        doc_version = "1.17.0",
        doc_homepage = "https://openucx.org/",
        strip_prefix = "ucx-1.17.0",
        urls = [
            "https://storage.googleapis.com/tensorstore-bazel-mirror/github.com/openucx/ucx/releases/download/v1.17.0/ucx-1.17.0.tar.gz",
            "https://github.com/openucx/ucx/releases/download/v1.17.0/ucx-1.17.0.tar.gz",
        ],
        sha256 = "34658e282f99f89ce7a991c542e9727552734ac6ad408c52f22b4c2653b04276",
        build_file = Label("//third_party/ucx:system.BUILD.bazel"),
        system_build_file = Label("//third_party/ucx:system.BUILD.bazel"),
        cmake_name = "UCX",
        bazel_to_cmake = {},
        cmake_target_mapping = {
            "//:ucp": "ucx::ucp",
            "//:uct": "ucx::uct",
            "//:ucs": "ucx::ucs",
        },
    ) 
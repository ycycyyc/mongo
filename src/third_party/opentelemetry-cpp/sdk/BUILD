# Copyright The OpenTelemetry Authors
# SPDX-License-Identifier: Apache-2.0

load("//bazel:mongo_src_rules.bzl", "mongo_cc_library")
load("//src/third_party/opentelemetry-cpp:otel_rules.bzl", "OTEL_TARGET_COMPATIBLE_WITH")

package(default_visibility = ["//visibility:public"])

mongo_cc_library(
    name = "headers",
    hdrs = glob(["include/**/*.h"]),
    includes = [
        # Any target depending on "//src/third_party/opentelemetry-cpp/sdk" also gets
        # "src/third_party/opentelemetry-cpp/sdk/include" added to the include path.
        "include",
    ],
    strip_include_prefix = "include",
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
)

// SPDX-License-Identifier: GPL-2.0-only
#include "torne_ts/profile.h"

#include <cstdio>
#include <string>

namespace {

namespace cli = px4::userland::cli;

int failures = 0;

#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

cli::Px4TsArguments parse_instance(const char* instance)
{
    const char* argv[] = {"torne-ts", "--instance", instance, "-c", "27"};
    return cli::parse_px4_ts_product_arguments(
        5, argv, torne::ts::kProfile);
}

void test_case_is_preserved()
{
    auto arguments = parse_instance("Abc");
    CHECK(arguments.valid);
    CHECK(torne::ts::normalize_instance(arguments));
    CHECK(arguments.instance == "Abc");

    arguments = parse_instance("abc");
    CHECK(arguments.valid);
    CHECK(torne::ts::normalize_instance(arguments));
    CHECK(arguments.instance == "abc");
}

void test_invalid_tokens_are_rejected_by_core_rules()
{
    const char* const invalid[] = {"com1", "COM1", ".abc", "abc.", "prn"};
    for (const char* instance : invalid) {
        auto arguments = parse_instance(instance);
        // Either the shared host_ts parse or normalize_instance may reject the
        // token; the product contract is that it never reaches the runner.
        CHECK(!arguments.valid || !torne::ts::normalize_instance(arguments));
    }
}

void test_ordinary_token_is_accepted()
{
    auto arguments = parse_instance("torne");
    CHECK(arguments.valid);
    CHECK(torne::ts::normalize_instance(arguments));
    CHECK(arguments.instance == "torne");
}

}  // namespace

int main()
{
    test_case_is_preserved();
    test_invalid_tokens_are_rejected_by_core_rules();
    test_ordinary_token_is_accepted();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    return 0;
}

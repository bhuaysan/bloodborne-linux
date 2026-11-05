// Differential tests of the string comparisons in src/game/text/compare.cpp.
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "game/text/compare.hpp"
#include "harness/eboot.hpp"
#include "harness/harness.hpp"

namespace {

using bb::harness::Arena;
using bb::harness::Result;
using bb::test::EbootTest;

using StartsWithFn = int(const char* const*, const char*);

// A pointer to `text` and `prefix`, both in the arena.
auto text_and_prefix(std::string_view text, std::string_view prefix) {
    return [text, prefix](Arena& a) {
        auto** holder = a.zeroed<const char*>();
        *holder = a.str(text);
        return std::tuple<const char* const*, const char*>{holder, a.str(prefix)};
    };
}

TEST_F(EbootTest, TextStartsWithMatches) {
    EXPECT_EQ(image().function_size(0x43e190), std::optional<std::uint64_t>{45});
    const std::pair<std::string_view, std::string_view> cases[] = {
        {"", ""},           {"abc", ""},     {"", "a"},         {"Skin 1", "Skin "},
        {"Skin", "Skin "},  {"Skin ", "Skin "}, {"skin 1", "Skin "}, {"abc", "abd"},
        {"\x80x", "\x80"},  {"\x7f", "\x80"},
    };
    for (const auto& [text, prefix] : cases) {
        const Result r = harness().compare<StartsWithFn>(0x43e190, &bb::game::text_starts_with,
                                                         text_and_prefix(text, prefix));
        EXPECT_TRUE(r) << '"' << text << "\" / \"" << prefix << "\": " << r.report();
    }
}

using Utf16EqualsFn = int(const void*, const char16_t*, const char16_t*, const char*);

// `wide` in the arena as [begin, end) and `narrow` as a NUL-terminated string. A sentinel unit
// follows the range, so a read past `end` would see a match for 'x'.
auto wide_and_narrow(std::u16string_view wide, std::string_view narrow) {
    return [wide, narrow](Arena& a) {
        auto* units = a.zeroed<char16_t>(wide.size() + 1);
        std::memcpy(units, wide.data(), wide.size() * sizeof(char16_t));
        units[wide.size()] = u'x';
        return std::tuple<const void*, const char16_t*, const char16_t*, const char*>{
            nullptr, units, units + wide.size(), a.str(narrow)};
    };
}

TEST_F(EbootTest, Utf16RangeEqualsMatches) {
    EXPECT_EQ(image().function_size(0x40e9e0), std::optional<std::uint64_t>{79});
    const std::pair<std::u16string_view, std::string_view> cases[] = {
        {u"", ""},       {u"", "a"},       {u"a", ""},        {u"abc", "abc"},
        {u"ab", "abc"},  {u"abc", "ab"},   {u"abc", "abd"},   {u"Ł", "A"},
        {u"é", "\xe9"}, {u"é", "\xc3\xa9"}, {u"䄀", "A"}, {u"ab", "abx"},
    };
    for (const auto& [wide, narrow] : cases) {
        const Result r = harness().compare<Utf16EqualsFn>(
            0x40e9e0, &bb::game::utf16_range_equals, wide_and_narrow(wide, narrow));
        EXPECT_TRUE(r) << wide.size() << " units / \"" << narrow << "\": " << r.report();
    }
}

}  // namespace

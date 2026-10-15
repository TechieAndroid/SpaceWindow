// A minimal, dependency-free unit-test harness.
//
//   TEST_CASE(Name) { CHECK(condition); CHECK_EQ(actual, expected); }
//   VOLUME_TEST_CASE(Name) { ... }   reads a drive directly: runs only with --volume
//
// A failed check throws; the runner reports it and moves on to the next test.
#pragma once

#include <cstdint>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace test
{
    struct TestCase
    {
        char const* name;
        void (*body)();
        bool readsVolume;  // needs administrator rights and takes a few seconds: run only when asked
    };

    std::vector<TestCase>& Registry();

    struct Registrar
    {
        Registrar(char const* name, void (*body)(), bool readsVolume = false)
        {
            Registry().push_back({ name, body, readsVolume });
        }
    };

    // Thrown by a failed check.
    struct Failure
    {
        std::string message;
    };

    std::string ToUtf8(std::wstring_view text);

    [[noreturn]] void Fail(std::string const& message, std::source_location location);

    template <typename T>
    std::string Describe(T const& value)
    {
        if constexpr (std::is_same_v<T, std::wstring> || std::is_same_v<T, std::wstring_view>)
        {
            return "L\"" + ToUtf8(value) + "\"";
        }
        else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>)
        {
            return "\"" + std::string(value) + "\"";
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            return value ? "true" : "false";
        }
        else if constexpr (std::is_enum_v<T>)
        {
            return std::to_string(static_cast<std::underlying_type_t<T>>(value));
        }
        else if constexpr (std::is_arithmetic_v<T>)
        {
            return std::to_string(value);
        }
        else
        {
            return "<value>";
        }
    }

    template <typename A, typename B>
    void CheckEqual(A const& actual, B const& expected, char const* actualText, char const* expectedText,
                    std::source_location location)
    {
        if (!(actual == expected))
        {
            Fail(std::string(actualText) + " == " + expectedText + "\n      actual:   " + Describe(actual) +
                     "\n      expected: " + Describe(expected),
                 location);
        }
    }
}

#define TEST_CASE(name)                                                    \
    static void name();                                                    \
    static ::test::Registrar const name##Registrar{ #name, &name };       \
    static void name()

#define VOLUME_TEST_CASE(name)                                             \
    static void name();                                                    \
    static ::test::Registrar const name##Registrar{ #name, &name, true }; \
    static void name()

#define CHECK(condition)                                                                    \
    do                                                                                      \
    {                                                                                       \
        if (!(condition))                                                                   \
        {                                                                                   \
            ::test::Fail("CHECK(" #condition ")", std::source_location::current());         \
        }                                                                                   \
    } while (false)

#define CHECK_EQ(actual, expected) \
    ::test::CheckEqual((actual), (expected), #actual, #expected, std::source_location::current())

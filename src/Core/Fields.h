// SpaceWindow - reading the fields of structures read straight from a disk.
//
// The master file table's records and the change journal's entries are read
// field by field at fixed offsets: little-endian, unaligned, and checked
// against the bytes they come from.  A field the bytes end before reads as
// nothing (Get) or as 0 (At).
#pragma once

#include <cstddef>
#include <cstring>
#include <optional>
#include <span>

namespace sm::fields
{
    template <typename T>
    bool Get(std::span<std::byte const> bytes, std::size_t offset, T& value) noexcept
    {
        if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
        {
            return false;
        }
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return true;
    }

    // A field, or 0 if the bytes end before it.
    template <typename T>
    T At(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
        T value{};
        (void)Get(bytes, offset, value);
        return value;
    }

    // The part of `bytes` at [offset, offset + length), or nothing if it does not fit.
    inline std::optional<std::span<std::byte const>> Slice(std::span<std::byte const> bytes, std::size_t offset,
                                                           std::size_t length) noexcept
    {
        if (offset > bytes.size() || bytes.size() - offset < length)
        {
            return std::nullopt;
        }
        return bytes.subspan(offset, length);
    }
}

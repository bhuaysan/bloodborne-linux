// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/common/concepts.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include <concepts>
#include <iterator>
#include <type_traits>

namespace Common {

// Check if type satisfies the ContiguousContainer named requirement.
template <typename T>
concept IsContiguousContainer = std::contiguous_iterator<typename T::iterator>;

template <typename Derived, typename Base>
concept DerivedFrom = std::derived_from<Derived, Base>;

// TODO: Replace with std::convertible_to when libc++ implements it.
template <typename From, typename To>
concept ConvertibleTo = std::is_convertible_v<From, To>;

// No equivalents in the stdlib

template <typename T>
concept IsArithmetic = std::is_arithmetic_v<T>;

template <typename T>
concept IsIntegral = std::is_integral_v<T>;

} // namespace Common

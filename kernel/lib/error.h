// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- fallible returns.
//
// The kernel has no exceptions, so every operation that can fail says so in
// its type. ErrorOr<T> carries either a value or an errno, and TRY() unwraps
// it or propagates the error to the caller. The point is that ignoring a
// failure has to be deliberate -- you cannot get at the value without first
// getting past the error.

#pragma once

#include <shitos/abi/errno.h>
#include <shitos/types.h>

namespace kernel {

class Error {
public:
    static constexpr Error from_errno(int code) { return Error(code); }

    constexpr int code() const { return m_code; }
    const char* to_string() const;

private:
    constexpr explicit Error(int code)
        : m_code(code)
    {
    }

    int m_code;
};

template<typename T>
class [[nodiscard]] ErrorOr {
public:
    constexpr ErrorOr(T value)
        : m_value(static_cast<T&&>(value))
        , m_is_error(false)
    {
    }

    constexpr ErrorOr(Error error)
        : m_error(error)
        , m_is_error(true)
    {
    }

    constexpr bool is_error() const { return m_is_error; }
    constexpr Error error() const { return m_error; }
    constexpr T& value() { return m_value; }
    constexpr T const& value() const { return m_value; }
    constexpr T release_value() { return static_cast<T&&>(m_value); }

private:
    union {
        T m_value;
        Error m_error;
    };
    bool m_is_error;
};

// The void specialisation still has to be checked; it just has nothing to hand
// back when it succeeds.
template<>
class [[nodiscard]] ErrorOr<void> {
public:
    constexpr ErrorOr()
        : m_error(Error::from_errno(ESUCCESS))
        , m_is_error(false)
    {
    }

    constexpr ErrorOr(Error error)
        : m_error(error)
        , m_is_error(true)
    {
    }

    constexpr bool is_error() const { return m_is_error; }
    constexpr Error error() const { return m_error; }
    constexpr void value() const { }
    constexpr void release_value() const { }

private:
    Error m_error;
    bool m_is_error;
};

} // namespace kernel

// Unwrap or propagate. Uses a statement expression, which both clang and gcc
// support and which is the only way to do this without exceptions.
#define TRY(expression)                             \
    ({                                              \
        auto&& _tmp_result = (expression);          \
        if (_tmp_result.is_error())                 \
            return _tmp_result.error();             \
        _tmp_result.release_value();                \
    })

// Unwrap or die. Only for cases where a failure means the kernel is already
// broken beyond repair -- never for anything a user can trigger.
#define MUST(expression)                                                     \
    ({                                                                       \
        auto&& _tmp_result = (expression);                                   \
        if (_tmp_result.is_error())                                          \
            ::kernel::panic("MUST(%s) failed: %s", #expression,              \
                _tmp_result.error().to_string());                            \
        _tmp_result.release_value();                                         \
    })

#define EINVAL_ERROR ::kernel::Error::from_errno(EINVAL)

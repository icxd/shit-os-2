// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- a growable array.
//
// Deliberately minimal: it grows, it shrinks, it does not throw (there are no
// exceptions), and every operation that can run out of memory says so in its
// return type rather than aborting. Anything that needs more than this
// probably wants an intrusive list instead.

#pragma once

#include <kernel/lib/error.h>
#include <kernel/lib/kstd.h>
#include <kernel/lib/new.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>

namespace kernel {

template<typename T>
class Vector : public NonCopyable {
public:
    constexpr Vector() = default;

    Vector(Vector&& other)
        : m_data(other.m_data)
        , m_size(other.m_size)
        , m_capacity(other.m_capacity)
    {
        other.m_data = nullptr;
        other.m_size = 0;
        other.m_capacity = 0;
    }

    ~Vector() { clear_and_free(); }

    usize size() const { return m_size; }
    usize capacity() const { return m_capacity; }
    bool is_empty() const { return m_size == 0; }

    T* data() { return m_data; }
    T const* data() const { return m_data; }

    T& operator[](usize index) { return m_data[index]; }
    T const& operator[](usize index) const { return m_data[index]; }

    T& last() { return m_data[m_size - 1]; }

    T* begin() { return m_data; }
    T* end() { return m_data + m_size; }
    T const* begin() const { return m_data; }
    T const* end() const { return m_data + m_size; }

    ErrorOr<void> reserve(usize wanted)
    {
        if (wanted <= m_capacity)
            return {};

        // Grow geometrically so that repeated appends stay amortised O(1).
        usize new_capacity = m_capacity == 0 ? 4 : m_capacity * 2;
        if (new_capacity < wanted)
            new_capacity = wanted;

        T* replacement = static_cast<T*>(kmalloc(new_capacity * sizeof(T)));
        if (replacement == nullptr)
            return Error::from_errno(ENOMEM);

        for (usize i = 0; i < m_size; ++i) {
            new (&replacement[i]) T(move(m_data[i]));
            m_data[i].~T();
        }

        kfree(m_data);
        m_data = replacement;
        m_capacity = new_capacity;
        return {};
    }

    ErrorOr<void> append(T value)
    {
        TRY(reserve(m_size + 1));
        new (&m_data[m_size]) T(move(value));
        ++m_size;
        return {};
    }

    void remove_at(usize index)
    {
        if (index >= m_size)
            return;
        m_data[index].~T();
        for (usize i = index; i + 1 < m_size; ++i) {
            new (&m_data[i]) T(move(m_data[i + 1]));
            m_data[i + 1].~T();
        }
        --m_size;
    }

    void clear()
    {
        for (usize i = 0; i < m_size; ++i)
            m_data[i].~T();
        m_size = 0;
    }

    void clear_and_free()
    {
        clear();
        kfree(m_data);
        m_data = nullptr;
        m_capacity = 0;
    }

private:
    T* m_data { nullptr };
    usize m_size { 0 };
    usize m_capacity { 0 };
};

} // namespace kernel

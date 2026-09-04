// SPDX-License-Identifier: GPL-3.0-or-later
//
// shit os 2 -- an intrusive doubly linked list.
//
// Intrusive rather than owning, because the things the kernel keeps in lists
// (threads, wait queues, inodes) outlive any particular list and must never
// require an allocation to be enqueued -- enqueuing from an interrupt handler
// that cannot allocate is a normal thing to want.

#pragma once

#include <kernel/lib/kstd.h>

#include <shitos/types.h>

namespace kernel {

template<typename T>
struct ListNode {
    T* previous { nullptr };
    T* next { nullptr };
    bool linked { false };
};

// `member` names the ListNode inside T, so one object can live in several
// lists at once by declaring several nodes.
template<typename T, ListNode<T> T::*member>
class IntrusiveList : public NonCopyable {
public:
    constexpr IntrusiveList() = default;

    bool is_empty() const { return m_head == nullptr; }
    usize size() const { return m_size; }
    T* first() const { return m_head; }
    T* last() const { return m_tail; }

    void append(T* item)
    {
        auto& node = item->*member;
        if (node.linked)
            return;

        node.previous = m_tail;
        node.next = nullptr;
        if (m_tail != nullptr)
            (m_tail->*member).next = item;
        else
            m_head = item;
        m_tail = item;
        node.linked = true;
        ++m_size;
    }

    void prepend(T* item)
    {
        auto& node = item->*member;
        if (node.linked)
            return;

        node.next = m_head;
        node.previous = nullptr;
        if (m_head != nullptr)
            (m_head->*member).previous = item;
        else
            m_tail = item;
        m_head = item;
        node.linked = true;
        ++m_size;
    }

    void remove(T* item)
    {
        auto& node = item->*member;
        if (!node.linked)
            return;

        if (node.previous != nullptr)
            (node.previous->*member).next = node.next;
        else
            m_head = node.next;

        if (node.next != nullptr)
            (node.next->*member).previous = node.previous;
        else
            m_tail = node.previous;

        node.previous = nullptr;
        node.next = nullptr;
        node.linked = false;
        --m_size;
    }

    T* take_first()
    {
        T* item = m_head;
        if (item != nullptr)
            remove(item);
        return item;
    }

    static T* next_of(T* item) { return (item->*member).next; }

    // Range-for support. Advancing is done before the body runs, so removing
    // the current item during iteration is safe.
    class Iterator {
    public:
        explicit Iterator(T* item)
            : m_item(item)
            , m_next(item != nullptr ? (item->*member).next : nullptr)
        {
        }

        T* operator*() const { return m_item; }
        bool operator!=(Iterator const& other) const { return m_item != other.m_item; }

        Iterator& operator++()
        {
            m_item = m_next;
            m_next = m_item != nullptr ? (m_item->*member).next : nullptr;
            return *this;
        }

    private:
        T* m_item;
        T* m_next;
    };

    Iterator begin() const { return Iterator(m_head); }
    Iterator end() const { return Iterator(nullptr); }

private:
    T* m_head { nullptr };
    T* m_tail { nullptr };
    usize m_size { 0 };
};

} // namespace kernel

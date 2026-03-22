#pragma once

#include <cstddef>
#include <cstdlib>
#include <cassert>
#include <atomic>
#include <new>

// ─────────────────────────────────────────────────────────────────────────────
// MemoryPool<T, N> — fixed-size slab allocator.
//
// Pre-allocates N objects of type T. Alloc/free are O(1) via a lock-free
// free-list (suitable for single-threaded or SPSC use cases).
//
// NOT safe for concurrent alloc from multiple threads (use one pool per thread
// or add a mutex if multi-threaded alloc is needed).
// ─────────────────────────────────────────────────────────────────────────────

template<typename T, size_t N>
class MemoryPool {
    struct Node {
        alignas(T) unsigned char storage[sizeof(T)];
        Node* next = nullptr;
    };

public:
    MemoryPool() {
        for (size_t i = 0; i < N; ++i) {
            nodes_[i].next = free_list_;
            free_list_ = &nodes_[i];
        }
    }

    ~MemoryPool() = default;

    template<typename... Args>
    T* alloc(Args&&... args) {
        if (!free_list_) return nullptr;
        Node* node = free_list_;
        free_list_ = node->next;
        ++in_use_;
        return new (node->storage) T(std::forward<Args>(args)...);
    }

    void free(T* ptr) {
        if (!ptr) return;
        ptr->~T();
        Node* node = reinterpret_cast<Node*>(reinterpret_cast<unsigned char*>(ptr));
        // Find which node this pointer belongs to
        // (works because storage is the first member of Node)
        node->next = free_list_;
        free_list_ = node;
        --in_use_;
    }

    size_t capacity()  const { return N; }
    size_t in_use()    const { return in_use_; }
    size_t available() const { return N - in_use_; }

private:
    Node   nodes_[N];
    Node*  free_list_ = nullptr;
    size_t in_use_    = 0;
};

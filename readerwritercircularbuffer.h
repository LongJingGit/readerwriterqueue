// ©2020 Cameron Desrochers.
// Distributed under the simplified BSD license (see the license file that
// should have come with this header).

// Provides a C++11 implementation of a single-producer, single-consumer wait-free concurrent
// circular buffer (fixed-size queue).

#pragma once

#include <utility>
#include <chrono>
#include <memory>
#include <cstdlib>
#include <cstdint>
#include <cassert>

// Note that this implementation is fully modern C++11 (not compatible with old MSVC versions)
// but we still include atomicops.h for its LightweightSemaphore implementation.
#include "atomicops.h"

#ifndef MOODYCAMEL_CACHE_LINE_SIZE
#define MOODYCAMEL_CACHE_LINE_SIZE 64
#endif

namespace moodycamel
{
    template <typename T>
    class BlockingReaderWriterCircularBuffer
    {
    public:
        typedef T value_type;

    public:
        explicit BlockingReaderWriterCircularBuffer(std::size_t capacity)
            : maxcap(capacity), mask(), rawData(), data(),
              slots_(new spsc_sema::LightweightSemaphore(static_cast<spsc_sema::LightweightSemaphore::ssize_t>(capacity))),
              items(new spsc_sema::LightweightSemaphore(0)),
              nextSlot(0), nextItem(0)
        {
            // Round capacity up to power of two to compute modulo mask.
            // 将 capcity 四舍五入至 2 的幂来计算 mask
            // Adapted from http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2

            /**
             * 值得注意的是，这里的 mask 就是使得 buffer 成为 circular buffer 的关键因素
             * eg: 15 & 15 == 15        16 & 15 == 0        17 & 15 == 1        18 & 15 == 2
             * 这样就可以保证随着 nextSlot 和 nextItem 的增加，可以访问到 buffer 中的不同元素。
             *
             * nextSlot 和 nextItem 的增长范围取决于用户需要入队和出队的元素数量，理论上可以无限增长，但是访问范围仍然限制在 capacity 范围内
             * eg: 15 & 15 == 15        31 & 15 == 15       47 & 15 == 15
             * 大数据量且不重启的情况下，也有超过范围的可能性？？ unsigned long 的范围是 0 ~ 4,294,967,295
             **/
            --capacity;
            capacity |= capacity >> 1;
            capacity |= capacity >> 2;
            capacity |= capacity >> 4;
            for (std::size_t i = 1; i < sizeof(std::size_t); i <<= 1)
                capacity |= capacity >> (i << 3);
            mask = capacity++;

            // std::alignment_of<T>::value  返回 T 类型内存对齐的大小
            // 在这里已经完成了内存分配，后续元素入队时候的内存分配都是 placement new 的方式
            // 分配内存的时候多分配了 std::alignment_of<T>::value - 1 的内存
            rawData = static_cast<char *>(std::malloc(capacity * sizeof(T) + std::alignment_of<T>::value - 1));
            data = align_for<T>(rawData);
        }

        BlockingReaderWriterCircularBuffer(BlockingReaderWriterCircularBuffer &&other)
            : maxcap(0), mask(0), rawData(nullptr), data(nullptr),
              slots_(new spsc_sema::LightweightSemaphore(0)),
              items(new spsc_sema::LightweightSemaphore(0)),
              nextSlot(), nextItem()
        {
            swap(other);
        }

        BlockingReaderWriterCircularBuffer(BlockingReaderWriterCircularBuffer const &) = delete;

        // Note: The queue should not be accessed concurrently while it's
        // being deleted. It's up to the user to synchronize this.
        ~BlockingReaderWriterCircularBuffer()
        {
            for (std::size_t i = 0, n = items->availableApprox(); i != n; ++i)
                reinterpret_cast<T *>(data)[(nextItem + i) & mask].~T();
            std::free(rawData);
        }

        BlockingReaderWriterCircularBuffer &operator=(BlockingReaderWriterCircularBuffer &&other) noexcept
        {
            swap(other);
            return *this;
        }

        BlockingReaderWriterCircularBuffer &operator=(BlockingReaderWriterCircularBuffer const &) = delete;

        // Swaps the contents of this buffer with the contents of another.
        // Not thread-safe.
        void swap(BlockingReaderWriterCircularBuffer &other) noexcept
        {
            std::swap(maxcap, other.maxcap);
            std::swap(mask, other.mask);
            std::swap(rawData, other.rawData);
            std::swap(data, other.data);
            std::swap(slots_, other.slots_);
            std::swap(items, other.items);
            std::swap(nextSlot, other.nextSlot);
            std::swap(nextItem, other.nextItem);
        }

        // Enqueues a single item (by copying it).
        // Fails if not enough room to enqueue.
        // Thread-safe when called by producer thread.
        // No exception guarantee (state will be corrupted) if constructor of T throws.
        bool try_enqueue(T const &item)
        {
            // 等待逻辑：
            // 1. 判断队列中空闲 slots 的数量是否大于0
            //     1.1 如果队列中空闲 slots 的数量大于0，则将队列中空闲 slot 的数量减一，然后返回 true
            //     1.2 如果队列中空闲 slots 的数量不大于 0，说明队列已满，直接返回 false
            if (!slots_->tryWait())
                return false;
            // 入队逻辑：
            // 1. 将下一个空闲 slot 位置加一
            // 2. new 元素
            // 3. 当前已入队元素加一
            inner_enqueue(item);
            return true;
        }

        // Enqueues a single item (by moving it, if possible).
        // Fails if not enough room to enqueue.
        // Thread-safe when called by producer thread.
        // No exception guarantee (state will be corrupted) if constructor of T throws.
        bool try_enqueue(T &&item)
        {
            if (!slots_->tryWait())
                return false;
            inner_enqueue(std::move(item));
            return true;
        }

        // Blocks the current thread until there's enough space to enqueue the given item,
        // then enqueues it (via copy).
        // Thread-safe when called by producer thread.
        // No exception guarantee (state will be corrupted) if constructor of T throws.
        void wait_enqueue(T const &item)
        {
            while (!slots_->wait())
                ;
            inner_enqueue(item);
        }

        // Blocks the current thread until there's enough space to enqueue the given item,
        // then enqueues it (via move, if possible).
        // Thread-safe when called by producer thread.
        // No exception guarantee (state will be corrupted) if constructor of T throws.
        void wait_enqueue(T &&item)
        {
            while (!slots_->wait())
                ;
            inner_enqueue(std::move(item));
        }

        // Blocks the current thread until there's enough space to enqueue the given item,
        // or the timeout expires. Returns false without enqueueing the item if the timeout
        // expires, otherwise enqueues the item (via copy) and returns true.
        // Thread-safe when called by producer thread.
        // No exception guarantee (state will be corrupted) if constructor of T throws.
        bool wait_enqueue_timed(T const &item, std::int64_t timeout_usecs)
        {
            if (!slots_->wait(timeout_usecs))
                return false;
            inner_enqueue(item);
            return true;
        }

        // Blocks the current thread until there's enough space to enqueue the given item,
        // or the timeout expires. Returns false without enqueueing the item if the timeout
        // expires, otherwise enqueues the item (via move, if possible) and returns true.
        // Thread-safe when called by producer thread.
        // No exception guarantee (state will be corrupted) if constructor of T throws.
        bool wait_enqueue_timed(T &&item, std::int64_t timeout_usecs)
        {
            if (!slots_->wait(timeout_usecs))
                return false;
            inner_enqueue(std::move(item));
            return true;
        }

        // Blocks the current thread until there's enough space to enqueue the given item,
        // or the timeout expires. Returns false without enqueueing the item if the timeout
        // expires, otherwise enqueues the item (via copy) and returns true.
        // Thread-safe when called by producer thread.
        // No exception guarantee (state will be corrupted) if constructor of T throws.
        template <typename Rep, typename Period>
        inline bool wait_enqueue_timed(T const &item, std::chrono::duration<Rep, Period> const &timeout)
        {
            return wait_enqueue_timed(item, std::chrono::duration_cast<std::chrono::microseconds>(timeout).count());
        }

        // Blocks the current thread until there's enough space to enqueue the given item,
        // or the timeout expires. Returns false without enqueueing the item if the timeout
        // expires, otherwise enqueues the item (via move, if possible) and returns true.
        // Thread-safe when called by producer thread.
        // No exception guarantee (state will be corrupted) if constructor of T throws.
        template <typename Rep, typename Period>
        inline bool wait_enqueue_timed(T &&item, std::chrono::duration<Rep, Period> const &timeout)
        {
            return wait_enqueue_timed(std::move(item), std::chrono::duration_cast<std::chrono::microseconds>(timeout).count());
        }

        // Attempts to dequeue a single item.
        // Returns false if the buffer is empty.
        // Thread-safe when called by consumer thread.
        // No exception guarantee (state will be corrupted) if assignment operator of U throws.
        template <typename U>
        bool try_dequeue(U &item)
        {
            if (!items->tryWait())
                return false;
            inner_dequeue(item);
            return true;
        }

        // Blocks the current thread until there's something to dequeue, then dequeues it.
        // Thread-safe when called by consumer thread.
        // No exception guarantee (state will be corrupted) if assignment operator of U throws.
        template <typename U>
        void wait_dequeue(U &item)
        {
            /*
             * 出队逻辑：
             * 1. 判断队列中已入队元素的数量是否大于0
             *      1.1 如果已入队元素数量大于 0，则将已入队元素数量减一
             *      1.2 如果已入队元素数量不大于 0，说明此时队列为空，则进入自旋，等待元素入队
             */

            while (!items->wait())
                ;
            inner_dequeue(item);
        }

        // Blocks the current thread until either there's something to dequeue
        // or the timeout expires. Returns false without setting `item` if the
        // timeout expires, otherwise assigns to `item` and returns true.
        // Thread-safe when called by consumer thread.
        // No exception guarantee (state will be corrupted) if assignment operator of U throws.
        template <typename U>
        bool wait_dequeue_timed(U &item, std::int64_t timeout_usecs)
        {
            if (!items->wait(timeout_usecs))
                return false;
            inner_dequeue(item);
            return true;
        }

        // Blocks the current thread until either there's something to dequeue
        // or the timeout expires. Returns false without setting `item` if the
        // timeout expires, otherwise assigns to `item` and returns true.
        // Thread-safe when called by consumer thread.
        // No exception guarantee (state will be corrupted) if assignment operator of U throws.
        template <typename U, typename Rep, typename Period>
        inline bool wait_dequeue_timed(U &item, std::chrono::duration<Rep, Period> const &timeout)
        {
            return wait_dequeue_timed(item, std::chrono::duration_cast<std::chrono::microseconds>(timeout).count());
        }

        // Returns a (possibly outdated) snapshot of the total number of elements currently in the buffer.
        // Thread-safe.
        inline std::size_t size_approx() const
        {
            return items->availableApprox();
        }

        // Returns the maximum number of elements that this circular buffer can hold at once.
        // Thread-safe.
        inline std::size_t max_capacity() const
        {
            return maxcap;
        }

    private:
        template <typename U>
        void inner_enqueue(U &&item)
        {
            std::size_t i = nextSlot++;
            // nextSlot 会不断递增，但是 & mask 之后仍然在 capacity 的范围内
            // std::cout << "nextSlot = " << nextSlot << std::endl;
            new (reinterpret_cast<T *>(data) + (i & mask)) T(std::forward<U>(item));
            // 队列中已入队元素加一
            items->signal();
        }

        template <typename U>
        void inner_dequeue(U &item)
        {
            std::size_t i = nextItem++;
            // std::cout << "nextItem = " << nextItem << std::endl;
            // nextItem 会不断递增，但是 & mask 之后仍然在 capacity 的范围内
            T &element = reinterpret_cast<T *>(data)[i & mask];
            item = std::move(element);
            element.~T();
            // 队列中的空闲 spot 加一
            slots_->signal();
        }

        template <typename U>
        static inline char *align_for(char *ptr)
        {
            const std::size_t alignment = std::alignment_of<U>::value;
            return ptr + (alignment - (reinterpret_cast<std::uintptr_t>(ptr) % alignment)) % alignment;
        }

    private:
        /*
         * 为什么队列的底层不使用普通链表结构：
         * ans: C++ 程序通常是对内存极度敏感的，如果队列的底层使用链表，动态的创建或者删除节点会导致内存的申请和释放，长时间运行会导致内存空洞的问题。
         * ans: 同时需要考虑队列长度一定要有上限设置，否则队列过大会出现内存问题。即两点要求：
         * 1. 上限设置
         * 2. 固定内存分配
         * 上面的要求很容易联想到循环队列，队列基于数组实现
         */
        std::size_t maxcap;                                      // actual (non-power-of-two) capacity
        std::size_t mask;                                        // circular buffer capacity mask (for cheap modulo)
        char *rawData;                                           // raw circular buffer memory
        char *data;                                              // circular buffer memory aligned to element alignment
        std::unique_ptr<spsc_sema::LightweightSemaphore> slots_; // number of slots currently free (named with underscore to accommodate Qt's 'slots' macro)
        std::unique_ptr<spsc_sema::LightweightSemaphore> items;  // number of elements currently enqueued
        char cachelineFiller0[MOODYCAMEL_CACHE_LINE_SIZE - sizeof(char *) * 2 - sizeof(std::size_t) * 2 - sizeof(std::unique_ptr<spsc_sema::LightweightSemaphore>) * 2];
        std::size_t nextSlot; // index of next free slot to enqueue into
        char cachelineFiller1[MOODYCAMEL_CACHE_LINE_SIZE - sizeof(std::size_t)];
        std::size_t nextItem; // index of next element to dequeue from
    };

}

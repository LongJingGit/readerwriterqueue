// Adapted from https://github.com/facebook/folly/blob/master/folly/ProducerConsumerQueue.h
/*
 * Copyright 2013 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// @author Bo Hu (bhu@fb.com)
// @author Jordan DeLong (delong.j@fb.com)

#ifndef PRODUCER_CONSUMER_QUEUE_H_
#define PRODUCER_CONSUMER_QUEUE_H_

#include <new>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <stdexcept>
#include <type_traits>
#include <utility>
//#include <boost/noncopyable.hpp>

namespace folly
{

    /*
     * ProducerConsumerQueue is a one producer and one consumer queue
     * without locks.
     */
    template <class T>
    struct ProducerConsumerQueue
    {
        typedef T value_type;

        // size must be >= 1.
        explicit ProducerConsumerQueue(uint32_t size)
            : size_(size + 1) // +1 because one slot is always empty
                              // +1 是为了即使只有生产者没有消费者的情况下，也能够容纳 size 的元素，并且有一个 slot为空
              ,
              records_(static_cast<T *>(std::malloc(sizeof(T) * (size + 1)))), readIndex_(0), writeIndex_(0)
        {
            assert(size >= 1);
            if (!records_)
            {
                throw std::bad_alloc();
            }
        }

        ~ProducerConsumerQueue()
        {
            // We need to destruct anything that may still exist in our queue.
            // (No real synchronization needed at destructor time: only one
            // thread can be doing this.)
            if (!std::is_trivially_destructible<T>::value)
            {
                int read = readIndex_;
                int end = writeIndex_;
                while (read != end)
                {
                    records_[read].~T();
                    if (++read == size_)
                    {
                        read = 0;
                    }
                }
            }

            std::free(records_);
        }

        template <class... Args>
        bool enqueue(Args &&...recordArgs)
        {
            /**
             * 为什么读取 writeIndex_ 只需要保证原子操作，不需要禁止指令重排？（即为什么这里没有使用其他的内存序？）
             *
             * 因为这里是 SPSC，只有一个生产者。只有生产者才会更新(写入) writeIndex_ 变量。
             * 所以当生产者读取 writeIndex_ 变量的时候，不会有其他线程更新该变量，所以不需要使用其他的内存序。
             */
            auto const currentWrite = writeIndex_.load(std::memory_order_relaxed);
            auto nextRecord = currentWrite + 1;
            if (nextRecord == size_)
            {
                nextRecord = 0;
            }

            /**
             * 入队操作读取 readIndex_ 使用了 acquire 内存序的原因：
             * readIndex_ 是由消费者线程更新的。当生产者线程在进行入队操作时，有可能有消费者线程在进行出队操作，
             * 所以必须保证 release 之前关于内存的写入操作对 acquire 之后对内存的读取操作可见。
             *
             * acquire 内存序必须和 load 操作搭配使用
             */
            if (nextRecord != readIndex_.load(std::memory_order_acquire))
            {
                new (&records_[currentWrite]) T(std::forward<Args>(recordArgs)...);
                // 对于同一个原子变量， release 之前的写入，一定对 acquire 之后的读取操作可见
                /**
                 * 写入 writeIndex_ 变量更新时需要和消费者线程读取该变量使用内存序进行同步
                 */
                writeIndex_.store(nextRecord, std::memory_order_release);
                return true;
            }

            // queue is full
            return false;
        }

        // move (or copy) the value at the front of the queue to given variable
        bool try_dequeue(T &record)
        {
            /**
             * 这里对 readIndex_ 的读取操作不使用其他内存序的原因和入队操作时读取 writeIndex_ 时不使用其他内存序的原因相同。
             */
            auto const currentRead = readIndex_.load(std::memory_order_relaxed);
            if (currentRead == writeIndex_.load(std::memory_order_acquire))
            {
                // queue is empty
                return false;
            }

            auto nextRecord = currentRead + 1;
            if (nextRecord == size_)
            {
                nextRecord = 0;
            }
            record = std::move(records_[currentRead]);
            records_[currentRead].~T();
            readIndex_.store(nextRecord, std::memory_order_release);
            return true;
        }

        // pointer to the value at the front of the queue (for use in-place) or
        // nullptr if empty.
        T *frontPtr()
        {
            auto const currentRead = readIndex_.load(std::memory_order_relaxed);
            if (currentRead == writeIndex_.load(std::memory_order_acquire))
            {
                // queue is empty
                return nullptr;
            }
            return &records_[currentRead];
        }

        // queue must not be empty
        void popFront()
        {
            auto const currentRead = readIndex_.load(std::memory_order_relaxed);
            assert(currentRead != writeIndex_.load(std::memory_order_acquire));

            auto nextRecord = currentRead + 1;
            if (nextRecord == size_)
            {
                nextRecord = 0;
            }
            records_[currentRead].~T();
            readIndex_.store(nextRecord, std::memory_order_release);
        }

        bool isEmpty() const
        {
            return readIndex_.load(std::memory_order_consume) ==
                   writeIndex_.load(std::memory_order_consume);
        }

        bool isFull() const
        {
            auto nextRecord = writeIndex_.load(std::memory_order_consume) + 1;
            if (nextRecord == size_)
            {
                nextRecord = 0;
            }
            if (nextRecord != readIndex_.load(std::memory_order_consume))
            {
                return false;
            }
            // queue is full
            return true;
        }

        // * If called by consumer, then true size may be more (because producer may
        //   be adding items concurrently).
        // * If called by producer, then true size may be less (because consumer may
        //   be removing items concurrently).
        // * It is undefined to call this from any other thread.
        size_t sizeGuess() const
        {
            int ret = writeIndex_.load(std::memory_order_consume) -
                      readIndex_.load(std::memory_order_consume);
            if (ret < 0)
            {
                ret += size_;
            }
            return ret;
        }

    private:
        const uint32_t size_; // 申请的reocords_ 的大小

        /**
         * 普通数组通过下标的控制，变成了基于循环数组实现的循环队列
         * 当 writeIndex_ + 1 == size_ 时，说明 writeIndex_ 到了数组的最后一个位置，需要从数组的第一个元素位置开始循环写入，所以 writeIndex_ = 0;
         * 同理，当 readIndex_ + 1 == size_ 时，readIndex_ = 0
         *
         * readIndex_ == writeIndex_ 时，队列空
         * writeIndex_ + 1 == readIndex_ 时，队列满
         **/
        T *const records_; // T 的数组

        std::atomic<int> readIndex_;  // 队头出队元素的位置
        std::atomic<int> writeIndex_; // 队尾入队元素的位置
    };

}

#endif

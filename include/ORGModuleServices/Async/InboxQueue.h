#pragma once

#include <atomic>
#include <mutex>
#include <utility>

namespace org::async {

// An unbounded multi-producer, multi-consumer FIFO (push order). Producers never wait: a push links a node onto a lock-free inbox. Consumers
// take a mutex, so a consumer that finds another one popping blocks instead of spinning.
//
// tbb::concurrent_queue spins (pause, then SwitchToThread) while an earlier popper or pusher of the same slot is preempted. Its
// workers at mixed priorities spun for seconds on a busy machine, starving the preempted thread and every other thread of the
// process (the slow phases, 2026-11-26). Nothing here waits on a preempted thread except the consumers' mutex, and that blocks.
template <class T>
class InboxQueue {
public:
    InboxQueue() = default;
    InboxQueue(const InboxQueue&) = delete;
    InboxQueue& operator=(const InboxQueue&) = delete;
    ~InboxQueue() {
        Free(inbox.load(std::memory_order_acquire));
        Free(taken);
    }

    void Push(T a_value) {
        auto* node = new Node{std::move(a_value), inbox.load(std::memory_order_relaxed)};
        while (!inbox.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed)) {}
    }

    bool TryPop(T& a_value) {
        Node* node = nullptr;
        {
            std::lock_guard lock(mutex);
            if (!taken) {
                // The inbox holds the newest push first, so reversing it gives push order.
                for (Node* next = inbox.exchange(nullptr, std::memory_order_acquire); next;) {
                    Node* const after = next->next;
                    next->next = taken;
                    taken = next;
                    next = after;
                }
                if (!taken)
                    return false;
            }
            node = taken;
            taken = node->next;
        }
        a_value = std::move(node->value);
        delete node;
        return true;
    }

private:
    struct Node {
        T value;
        Node* next = nullptr;
    };
    static void Free(Node* a_node) {
        while (a_node) {
            Node* const next = a_node->next;
            delete a_node;
            a_node = next;
        }
    }

    std::atomic<Node*> inbox{nullptr};  // pushed and not yet taken, newest first
    std::mutex mutex;
    Node* taken = nullptr;  // taken from the inbox, oldest first; under the mutex
};

} // namespace org::async

#pragma once
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <thread>
#include <tuple>
#include <utility>

namespace channel {

template <typename T> struct entity {
    std::size_t capacity = std::numeric_limits<std::size_t>::max();
    std::atomic<bool> closed;
    std::queue<T> queue;
    std::mutex mutex;
    std::mutex shared_ptr_mutex;
    std::condition_variable producers;
    std::condition_variable consumers;
};

template <typename T> class basic_chan {
  public:
    basic_chan() : e(std::make_shared<entity<T>>()){};

    explicit basic_chan(std::size_t capacity) : e(std::make_shared<entity<T>>()) {
        e->capacity = capacity;
    };

    basic_chan(const basic_chan<T> &c) {
        std::lock_guard<std::mutex> lock(c.e->shared_ptr_mutex);
        e = c.e;
    }

    basic_chan(basic_chan<T> &&c) {
        std::lock_guard<std::mutex> lock(c.e->shared_ptr_mutex);
        e = std::move_if_noexcept(c.e);
    }

    basic_chan<T> &operator=(const basic_chan<T> &c) {
        std::lock_guard<std::mutex> lock_rhs(c.e->shared_ptr_mutex);
        std::lock_guard<std::mutex> lock_lhs(e->shared_ptr_mutex);
        e = c.e;
    }

    basic_chan<T> &operator=(basic_chan<T> &&c) {
        std::lock_guard<std::mutex> lock_rhs(c.e->shared_ptr_mutex);
        std::lock_guard<std::mutex> lock_lhs(e->shared_ptr_mutex);
        e = std::move_if_noexcept(c.e);
    }

    virtual void close() {
        e->closed.store(true);
        e->consumers.notify_all();
    }

    virtual bool is_open() const { return !e->closed.load(); }

  protected:
    std::shared_ptr<entity<T>> e;
};

class channel_error : public std::runtime_error {
  public:
    channel_error(const char *what_arg) : std::runtime_error(what_arg) {}
};

template <typename T> class sender : virtual public basic_chan<T> {
  public:
    sender() = default;

    explicit sender(std::size_t capacity) : basic_chan<T>(capacity){};

    sender(const sender<T> &) = default;

    sender(sender<T> &&) = default;

    sender<T> &operator=(const sender<T> &) = default;

    sender<T> &operator=(sender<T> &&) = default;

    void send(const T &value) {
        if (!this->is_open())
            throw channel_error("send on closed channel");
        std::unique_lock<std::mutex> lock(this->e->mutex);
        this->e->producers.wait(lock,
                                [this] { return this->e->queue.size() < this->e->capacity; });
        this->e->queue.push(value);
        this->e->consumers.notify_one();
    }

    sender<T> &operator<<(const T &value) {
        send(value);
        return *this;
    }
};

template <typename T> class receiver : virtual public basic_chan<T> {
  private:
    template <typename U> class iterator {
      public:
        iterator(const receiver<U> &channel) : channel(channel), value(channel.receive()) {}

        iterator(const receiver<U> &channel, std::nullptr_t) : channel(channel) {}

        U &operator*() {
            if (!value)
                throw channel_error("receive on empty closed channel");
            return *value;
        }

        iterator<U> &operator++() {
            value = channel.receive();
            return *this;
        }

        bool operator!=(std::nullptr_t) const { return value; }

      protected:
        receiver<U> channel;
        std::optional<U> value;
    };

  public:
    receiver() = default;

    explicit receiver(std::size_t capacity) : basic_chan<T>(capacity){};

    receiver(const receiver<T> &) = default;

    receiver(receiver<T> &&) = default;

    receiver<T> &operator=(const receiver<T> &) = default;

    receiver<T> &operator=(receiver<T> &&) = default;

    std::optional<T> receive() {
        std::unique_lock<std::mutex> lock(this->e->mutex);
        this->e->consumers.wait(lock,
                                [this] { return !this->e->queue.empty() || !this->is_open(); });
        if (this->e->queue.empty())
            return std::optional<T>();
        std::optional<T> value(std::in_place, std::move_if_noexcept(this->e->queue.front()));
        this->e->queue.pop();
        this->e->producers.notify_one();
        return value;
    }

    std::optional<T> try_receive() {
        std::lock_guard<std::mutex> lock(this->e->mutex);
        if (this->e->queue.empty())
            return std::optional<T>();
        std::optional<T> value(std::in_place, std::move_if_noexcept(this->e->queue.front()));
        this->e->queue.pop();
        this->e->producers.notify_one();
        return value;
    }

    bool operator>>(T &value) {
        std::unique_lock<std::mutex> lock(this->e->mutex);
        this->e->consumers.wait(lock,
                                [this] { return !this->e->queue.empty() || !this->is_open(); });
        if (this->e->queue.empty())
            return false;
        value = std::move_if_noexcept(this->e->queue.front());
        this->e->queue.pop();
        this->e->producers.notify_one();
        return true;
    }

    iterator<T> begin() { return iterator<T>(*this); }

    std::nullptr_t end() { return nullptr; }
};

template <typename T> class chan : virtual public sender<T>, virtual public receiver<T> {
  public:
    chan() = default;

    explicit chan(std::size_t capacity) : basic_chan<T>(capacity){};
};

// reference: https://stackoverflow.com/a/28998020
template <size_t N> struct visit_impl {
    template <typename T, typename F> static void visit(T &tup, size_t idx, F func) {
        if (idx == N - 1)
            func(std::get<N - 1>(tup));
        else
            visit_impl<N - 1>::visit(tup, idx, func);
    }
};

template <> struct visit_impl<0> {
    template <typename T, typename F> static void visit(T &, size_t, F) { assert(false); }
};

template <typename... Ts>
void select(std::pair<receiver<Ts>, std::function<bool(Ts)>>... rfs,
            std::function<bool(void)> f = std::function<bool(void)>()) {
    auto rft = std::make_tuple(rfs...);
    std::random_device rd;
    std::uniform_int_distribution<std::size_t> dist(0, sizeof...(Ts) - 1);
    bool closed[sizeof...(Ts)] = {false};
    std::size_t not_closed = sizeof...(Ts);

    for (bool ctn = true; ctn && not_closed > 0;) {
        bool visited[sizeof...(Ts)] = {false};
        std::size_t not_visited = sizeof...(Ts);
        std::size_t idx;
        bool received;
        do {
            do {
                idx = dist(rd);
            } while (closed[idx]);
            visit_impl<sizeof...(Ts)>::visit(rft, idx, [&](auto &rf) {
                auto stage = rf.first.try_receive();
                received = bool(stage);
                if (received) {
                    ctn = rf.second(std::move_if_noexcept(*stage));
                    return;
                }
                if (f && !visited[idx]) {
                    visited[idx] = true;
                    --not_visited;
                }
                if (!rf.first.is_open()) {
                    closed[idx] = true;
                    --not_closed;
                }
            });
        } while (!received && not_closed > 0 && (!f || not_visited > 0));
        if (!received && f)
            ctn = f();
    }
}

using time_point = std::chrono::time_point<std::chrono::system_clock>;

template <typename T, typename U>
receiver<time_point> after(const std::chrono::duration<T, U> &period) {
    chan<time_point> channel;
    std::thread th([=]() mutable {
        std::this_thread::sleep_for(period);
        channel.send(std::chrono::system_clock::now());
        channel.close();
    });
    th.detach();
    return channel;
}

template <typename T, typename U>
receiver<time_point> tick(const std::chrono::duration<T, U> &interval) {
    chan<time_point> channel;
    std::thread th([=]() mutable {
        while (true) {
            std::this_thread::sleep_for(interval);
            try {
                channel.send(std::chrono::system_clock::now());
            } catch (const channel_error &) {
                break;
            }
        }
    });
    th.detach();
    return channel;
}

} // namespace channel

#pragma once
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <thread>
#include <tuple>

namespace channel {

template <typename T> struct entity {
    std::size_t capacity = std::numeric_limits<std::size_t>::max();
    std::atomic<bool> closed;
    std::queue<T> queue;
    std::mutex mutex;
    std::mutex shared_ptr_mutex;
    std::condition_variable produce;
    std::condition_variable consume;
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
        e->consume.notify_all();
    }

    virtual bool is_open() const { return !e->closed.load(); }

  protected:
    std::shared_ptr<entity<T>> e;
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
            throw std::runtime_error("send on closed channel");
        std::unique_lock<std::mutex> lock(this->e->mutex);
        this->e->produce.wait(lock, [this] { return this->e->queue.size() < this->e->capacity; });
        this->e->queue.push(value);
        this->e->consume.notify_one();
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
                throw std::runtime_error("receive on empty closed channel");
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
        this->e->consume.wait(lock,
                              [this] { return !this->e->queue.empty() || !this->is_open(); });
        if (this->e->queue.empty())
            return std::optional<T>();
        std::optional<T> value(std::in_place, std::move_if_noexcept(this->e->queue.front()));
        this->e->queue.pop();
        this->e->produce.notify_one();
        return value;
    }

    std::optional<T> try_receive() {
        std::lock_guard<std::mutex> lock(this->e->mutex);
        if (this->e->queue.empty())
            return std::optional<T>();
        std::optional<T> value(std::in_place, std::move_if_noexcept(this->e->queue.front()));
        this->e->queue.pop();
        this->e->produce.notify_one();
        return value;
    }

    bool operator>>(T &value) {
        std::unique_lock<std::mutex> lock(this->e->mutex);
        this->e->consume.wait(lock,
                              [this] { return !this->e->queue.empty() || !this->is_open(); });
        if (this->e->queue.empty())
            return false;
        value = std::move_if_noexcept(this->e->queue.front());
        this->e->queue.pop();
        this->e->produce.notify_one();
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

template <typename... Ts> class select {
  private:
    // reference: https://stackoverflow.com/a/28998020
    template <size_t N> struct visit_impl {
        template <typename T, typename U, typename F>
        static void visit(T &x, U &y, size_t idx, const F func) {
            if (idx == N - 1)
                func(std::get<N - 1>(x), std::get<N - 1>(y));
            else
                visit_impl<N - 1>::visit(x, y, idx, func);
        }
    };

    template <> struct visit_impl<0> {
        template <typename T, typename U, typename F>
        static void visit(T &, U &, size_t, const F) {
            assert(false);
        }
    };

    template <typename... Us> class iterator {
      public:
        iterator(const std::tuple<receiver<Us>...> &chans, bool nullable)
            : chans(chans), nullable(nullable), dist(0, sizeof...(Us) - 1) {
            ++*this;
        }

        iterator<Us...> &operator++() {
            stage = std::tuple<std::optional<Us>...>();
            std::size_t idx;
            std::size_t not_visited = sizeof...(Us);
            bool visited[sizeof...(Us)] = {false};
            bool received;
            do {
                do {
                    idx = dist(rd);
                } while (is_closed[idx]);
                visit_impl<sizeof...(Us)>::visit(stage, chans, idx,
                                                 [&, this](auto &stg, auto &receiver) {
                                                     stg = receiver.try_receive();
                                                     received = bool(stg);
                                                     if (!received) {
                                                         if (this->nullable) {
                                                             if (!visited[idx]) {
                                                                 visited[idx] = true;
                                                                 --not_visited;
                                                             }
                                                         } else if (!receiver.is_open()) {
                                                             this->is_closed[idx] = true;
                                                             --this->opened;
                                                         }
                                                     }
                                                 });
            } while (!received && opened > 0 && (!nullable || not_visited));
            return *this;
        }

        std::tuple<std::optional<Us>...> operator*() { return std::move_if_noexcept(stage); }

        bool operator!=(std::nullptr_t) const { return opened > 0; }

      private:
        std::tuple<receiver<Us>...> chans;
        bool nullable;
        std::tuple<std::optional<Us>...> stage;
        bool is_closed[sizeof...(Us)] = {false};
        std::size_t opened = sizeof...(Us);
        std::random_device rd;
        std::uniform_int_distribution<std::size_t> dist;
    };

    std::tuple<receiver<Ts>...> chans;
    bool nullable;

  public:
    select(const receiver<Ts> &...chans, bool nullable = false)
        : chans(std::make_tuple(chans...)), nullable(nullable) {}

    iterator<Ts...> begin() { return iterator<Ts...>(chans, nullable); }

    std::nullptr_t end() { return nullptr; }
};

using time_point = std::chrono::time_point<std::chrono::system_clock>;

template <typename T, typename U>
receiver<time_point> timer(const std::chrono::duration<T, U> &period) {
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
receiver<time_point> ticker(const std::chrono::duration<T, U> &interval) {
    chan<time_point> channel;
    std::thread th([=]() mutable {
        while (true) {
            std::this_thread::sleep_for(interval);
            try {
                channel.send(std::chrono::system_clock::now());
            } catch (const std::runtime_error &) {
                break;
            }
        }
    });
    th.detach();
    return channel;
}

} // namespace channel

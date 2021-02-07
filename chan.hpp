#pragma once
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include <tuple>

namespace chan {

template <typename T> class basic_chan {
  public:
    using element_type = T;

    basic_chan() = default;

    basic_chan(const basic_chan<T> &) = delete;

    basic_chan<T> &operator=(const basic_chan &) = delete;

    virtual void close() {
        closed.store(true);
        cv.notify_all();
    }

    virtual bool is_open() const { return !closed.load(); }

  protected:
    std::atomic<bool> closed;
    std::queue<T> queue;
    std::mutex mutex;
    std::condition_variable_any cv;
};

template <typename T> class sender : virtual public basic_chan<T> {
  public:
    void send(const T &value) {
        if (this->closed.load())
            throw std::runtime_error("send on closed channel");
        {
            std::lock_guard<std::mutex> lock(this->mutex);
            this->queue.push(value);
        }
        this->cv.notify_one();
    }

    sender<T> &operator<<(const T &value) {
        send(value);
        return *this;
    }
};

template <typename T> class receiver : virtual public basic_chan<T> {
  private:
    template <typename U> class box {
      public:
        box(receiver<U> &recver) : recver(recver), value(recver.receive()) {}

        box(receiver<U> &recver, std::nullptr_t) : recver(recver) {}

        U &operator*() {
            if (!value)
                throw std::runtime_error("receive on empty closed channel");
            return *value;
        }

        box<U> &operator++() {
            value = recver.receive();
            return *this;
        }

        bool operator!=(const box<U> &bx) const {
            return !(&recver == &bx.recver && !value && !bx.value);
        }

      protected:
        receiver<U> &recver;
        std::unique_ptr<U> value;
    };

  public:
    std::unique_ptr<T> receive() {
        std::unique_lock<std::mutex> lock(this->mutex);
        this->cv.wait(lock, [this] { return !this->queue.empty() || this->closed.load(); });
        if (this->queue.empty())
            return std::unique_ptr<T>();
        std::unique_ptr<T> value(new T(std::move_if_noexcept(this->queue.front())));
        this->queue.pop();
        return value;
    }

    std::unique_ptr<T> try_receive() {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (this->queue.empty())
            return std::unique_ptr<T>();
        std::unique_ptr<T> value(new T(std::move_if_noexcept(this->queue.front())));
        this->queue.pop();
        return value;
    }

    bool operator>>(T &value) {
        std::unique_lock<std::mutex> lock(this->mutex);
        this->cv.wait(lock, [this] { return !this->queue.empty() || this->closed.load(); });
        if (this->queue.empty())
            return false;
        value = std::move_if_noexcept(this->queue.front());
        this->queue.pop();
        return true;
    }

    box<T> begin() { return box<T>(*this); }

    box<T> end() { return box<T>(*this, nullptr); }
};

template <typename T> class chan : virtual public sender<T>, virtual public receiver<T> {};

template <typename... Ts> class chans {
  private:
    template <size_t N> struct visit_impl {
        template <typename T, typename U, typename F>
        static void visit(T &x, U &y, size_t i, const F func) {
            if (i == N - 1)
                func(std::get<N - 1>(x), std::get<N - 1>(y));
            else
                visit_impl<N - 1>::visit(x, y, i, func);
        }
    };

    template <> struct visit_impl<0> {
        template <typename T, typename U, typename F>
        static void visit(T &, U &, size_t, const F) {
            assert(false);
        }
    };

    template <typename... Us> class selector {
      public:
        selector(const std::tuple<receiver<Us> *...> &channels, bool nullable)
            : channels(channels), nullable(nullable), dist(0, sizeof...(Us) - 1) {
            ++*this;
        }

        selector(const selector<Us...> &) = delete;

        selector<Us...> &operator=(const selector<Us...> &) = delete;

        selector<Us...> &operator++() {
            std::size_t i, not_visited = sizeof...(Us);
            bool visited[sizeof...(Us)] = {false};
            bool received;
            do {
                do {
                    i = dist(rd);
                } while (is_closed[i]);
                visit_impl<sizeof...(Us)>::visit(
                    stage, channels, i, [&, this](auto &stg, auto &receiver) {
                        if (!(received = bool(stg = receiver->try_receive()))) {
                            if (this->nullable) {
                                if (!visited[i]) {
                                    visited[i] = true;
                                    --not_visited;
                                }
                            } else if (!receiver->is_open()) {
                                this->is_closed[i] = true;
                                --this->opened;
                            }
                        }
                    });
            } while (!received && opened && (!nullable || not_visited));
            return *this;
        }

        std::tuple<std::unique_ptr<Us>...> operator*() { return std::move(stage); }

        bool operator!=(std::nullptr_t) const { return opened != 0; }

      private:
        std::tuple<receiver<Us> *...> channels;
        bool nullable;
        std::tuple<std::unique_ptr<Us>...> stage;
        bool is_closed[sizeof...(Us)] = {false};
        int opened = sizeof...(Us);
        std::random_device rd;
        std::uniform_int_distribution<std::size_t> dist;
    };

    std::tuple<receiver<Ts> *...> channels;
    bool nullable;

  public:
    chans(receiver<Ts> &...channels, bool nullable = false)
        : channels(std::make_tuple(&channels...)), nullable(nullable) {}

    selector<Ts...> begin() { return selector<Ts...>(channels, nullable); }

    static std::nullptr_t end() { return nullptr; }
};

typedef std::chrono::time_point<std::chrono::system_clock> time_point;

template <typename T, typename U>
std::shared_ptr<receiver<time_point>> timer(const std::chrono::duration<T, U> &period) {
    std::shared_ptr<chan<time_point>> channel(new chan<time_point>);
    std::thread th([=] {
        std::this_thread::sleep_for(period);
        *channel << std::chrono::system_clock::now();
        channel->close();
    });
    th.detach();
    return std::move(channel);
}

template <typename T, typename U>
std::shared_ptr<receiver<time_point>> ticker(const std::chrono::duration<T, U> &interval) {
    std::shared_ptr<chan<time_point>> channel(new chan<time_point>);
    std::thread th([=] {
        while (true) {
            std::this_thread::sleep_for(interval);
            try {
                *channel << std::chrono::system_clock::now();
            } catch (const std::runtime_error &) {
                break;
            }
        }
    });
    th.detach();
    return std::move(channel);
}

} // namespace chan

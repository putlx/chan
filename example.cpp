#include "chan.hpp"
#include <cassert>
#include <chrono>
#include <thread>

static constexpr int size = 128;

void worker(chan::sender<int> &channel, chan::receiver<bool> &quit) {
    for (int m = 0; m < size; ++m) {
        std::thread th(
            [&](int m) {
                std::this_thread::sleep_for(std::chrono::milliseconds(m));
                channel << m;
            },
            m);
        th.detach();
    }
    quit.receive();
    channel.close();
}

int main() {
    chan::chan<int> channel;
    chan::chan<bool> quit;
    std::thread th(worker, std::ref(channel), std::ref(quit));
    th.detach();

    bool box[size] = {false};
    auto rest = size;
    auto timer = chan::timer(std::chrono::seconds(2));
    for (auto [m, t] : chan::chans<int, chan::time_point>(channel, *timer))
        if (m) {
            int &msg = *m;
            assert(msg >= 0 && msg < size && !box[msg]);
            box[msg] = true;
            if (!--rest)
                quit << true;
        }

    return 0;
}

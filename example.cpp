#include "chan.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

static constexpr int size = 16;

void worker(int n, channel::sender<int> chan, channel::sender<std::string> logger,
            channel::receiver<int> quit, channel::sender<int> exit) {
    chan << n;
    logger << "send " + std::to_string(n);
    assert(quit.receive());
    exit << n;
    logger << "worker " + std::to_string(n) + " exits";
}

int main() {
    channel::chan<int> chan(4), quit, exit;
    channel::chan<std::string> logger;
    std::thread ths[size];
    for (auto n = 0; n < size; ++n)
        ths[n] = std::thread(worker, n, chan, logger, quit, exit);

    bool ns[size] = {false};
    bool qs[size] = {false};
    auto ticker = channel::tick(std::chrono::seconds(1));
    const auto start_time = std::chrono::system_clock::now();

    channel::select<int, std::string, int, channel::time_point, channel::time_point>(
        {chan,
         [&](int n) {
             assert(!ns[n]);
             logger << "receive " + std::to_string(n);
             std::this_thread::sleep_for(std::chrono::milliseconds(400));
             ns[n] = true;
             quit << 0;
             return true;
         }},

        {logger,
         [&](std::string msg) {
             std::cout << msg << std::endl;
             return true;
         }},

        {exit,
         [&](int q) {
             assert(!qs[q]);
             qs[q] = true;
             ths[q].join();
             if (std::all_of(qs, qs + size, [](bool q) { return q; })) {
                 chan.close();
                 logger.close();
                 exit.close();
                 ticker.close();
             }
             return true;
         }},

        {channel::after(std::chrono::seconds(4), [&] { logger << "hello"; }),
         [&](channel::time_point now) {
             auto t = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
             std::cout << "after " << t << " seconds\n";
             return true;
         }},

        {ticker,
         [](channel::time_point) {
             std::cout << "1 second passed\n";
             return true;
         }},

        [] {
            std::cout << "nothing to do\n";
            return true;
        });

    assert(std::all_of(ns, ns + size, [](bool n) { return n; }));
    return 0;
}

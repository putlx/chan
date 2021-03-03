#include "chan.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <ctime>
#include <iostream>
#include <string>
#include <thread>
using namespace channel;

static constexpr int size = 16;

void worker(int n, sender<int> channel, sender<std::string> logger, receiver<bool> quit,
            sender<int> exit) {
    channel << n;
    logger << "send " + std::to_string(n);
    assert(quit.receive());
    exit << n;
    logger << "worker " + std::to_string(n) + " exit";
}

int main() {
    chan<int> channel(4);
    chan<std::string> logger;
    chan<bool> quit;
    chan<int> exit;
    std::thread ths[size];
    for (auto n = 0; n < size; ++n)
        ths[n] = std::thread(worker, n, channel, logger, quit, exit);

    auto tmr = timer(std::chrono::seconds(4));
    auto clk = ticker(std::chrono::seconds(1));
    bool ns[size] = {false};
    bool es[size] = {false};
    for (auto [n, msg, e, tp, tk] :
         select<int, std::string, int, time_point, time_point>(channel, logger, exit, tmr, clk)) {
        if (n) {
            assert(!ns[*n]);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            ns[*n] = true;
            logger << "receive " + std::to_string(*n);
            quit << true;
        } else if (msg) {
            std::cout << *msg << std::endl;
        } else if (e) {
            assert(!es[*e]);
            es[*e] = true;
            ths[*e].join();
            if (std::all_of(es, es + size, [](auto e) { return e; })) {
                channel.close();
                exit.close();
                logger.close();
                clk.close();
            }
        } else if (tp) {
            auto tt = std::chrono::system_clock::to_time_t(*tp);
            std::cout << "current time: " << std::ctime(&tt);
        } else if (tk) {
            std::cout << "1 second passed" << std::endl;
        }
    }

    assert(std::all_of(ns, ns + size, [](auto n) { return n; }));
    return 0;
}

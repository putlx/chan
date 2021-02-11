#include "chan.hpp"
#include <chrono>
#include <ctime>
#include <iostream>
#include <string>
#include <thread>
using namespace channel;

static constexpr int size = 16;

void worker(int i, sender<int> &channel, sender<std::string> &logger, receiver<bool> &quit) {
    auto m = 1 << i;
    channel << m;
    logger << "send " + std::to_string(m);
    quit.receive();
    logger << "worker " + std::to_string(i) + " exit";
}

int main() {
    chan<int> channel(4);
    chan<std::string> logger;
    chan<bool> quit;
    for (int i = 1; i <= size; ++i) {
        std::thread th(worker, i, std::ref(channel), std::ref(logger), std::ref(quit));
        th.detach();
    }

    auto logs = size * 3;
    auto tm = timer(std::chrono::seconds(4));
    for (auto [msg, log, t] : select<int, std::string, time_point>(channel, logger, *tm))
        if (msg) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            logger << "receive " + std::to_string(*msg);
            quit << true;
        } else if (log) {
            std::cout << *log << std::endl;
            if (!--logs) {
                channel.close();
                logger.close();
            }
        } else if (t) {
            auto time = std::chrono::system_clock::to_time_t(*t);
            std::cout << "current time: " << std::ctime(&time);
        }

    return 0;
}

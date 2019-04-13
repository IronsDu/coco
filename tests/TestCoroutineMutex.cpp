#include <iostream>

#include <asio/signal_set.hpp>
#include <asio/detached.hpp>
#include <asio/co_spawn.hpp>

#include "coco/Utils.h"
#include "coco/CoroutineMutex.h"

using namespace coco;

std::atomic<int> global = 0;

static awaitable<void> testMutex(CoroutineMutex::Ptr mutex, int index)
{
    for (int i = 0; i < 10; i++)
    {
        co_await Utils::Sleep(mutex->context(), std::chrono::milliseconds(100 + std::rand() % 100));

        co_await mutex->lock();
        
        global++;
        std::cout << "in " << index << " update global value to " << global << std::endl;
        co_await Utils::Sleep(mutex->context(), std::chrono::milliseconds(100 + std::rand() % 100));

        mutex->unlock();
    }
}

int main()
{
    try
    {
        asio::io_context io_context(1);

        asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&](auto, auto) { io_context.stop(); });

        auto mutex = CoroutineMutex::Make(io_context);
        for (int i = 0; i < 10; i++)
        {
            co_spawn(io_context, [=]() {
                return testMutex(mutex, i);
            }, detached);
        }

        io_context.run();
    }
    catch (std::exception & e)
    {
        std::printf("Exception: %s\n", e.what());
    }
}
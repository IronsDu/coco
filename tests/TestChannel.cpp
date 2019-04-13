#include <iostream>

#include <asio/signal_set.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include "coco/Channel.h"
#include "coco/Utils.h"

using namespace coco;

static awaitable<void> producer(Channel<int> channel)
{
    int i = 0;

    while (true)
    {
        co_await Utils::Sleep(channel.context(), std::chrono::seconds(1));
        channel << i++;
    }
}

static awaitable<void> consumer(Channel<int> channel)
{
    while (true)
    {
        {
            auto i = co_await channel.takeOne();
            std::cout << i << std::endl;
        }
        {
            auto v = co_await channel.takeAll();
            for (; !v.empty();)
            {
                std::cout << v.front() << std::endl;
                v.pop();
            }
        }
    }
}

int main()
{
    try
    {
        asio::io_context io_context(1);

        asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&](auto, auto) { io_context.stop(); });

        // 一个生产者协程和两个消费者协程
        auto channel = Channel<int>::Make(io_context);
        co_spawn(io_context, [=]() {
            return producer(channel);
        }, asio::detached);
        co_spawn(io_context, [=]() {
            return consumer(channel);
        }, asio::detached);
        co_spawn(io_context, [=]() {
            return consumer(channel);
        }, asio::detached);

        io_context.run();
    }
    catch (std::exception & e)
    {
        std::printf("Exception: %s\n", e.what());
    }
}
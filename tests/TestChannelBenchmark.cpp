#include <iostream>

#include <asio/signal_set.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include "coco/Channel.h"
#include "coco/Utils.h"

using namespace coco;
const static auto MaxNum = 200000;
static int CurrentValue = 0;

static awaitable<void> foo(Channel<int> in, Channel<int> out)
{
    for (;;)
    {
        auto value = co_await in.takeOne();
        // 将收到的值+1传递给下一位伙伴
        CurrentValue = value;
        out << (value + 1);
    }
}

static awaitable<void> start(Channel<int> out)
{
    out << 1;
    for (;;)
    {
        auto old = CurrentValue;
        co_await Utils::Sleep(out.context(), std::chrono::seconds(1));
        std::cout << "current value:" << CurrentValue << ", diff:" << (CurrentValue-old) << std::endl;
    }
    co_return;
}

int main()
{
    try
    {
        asio::io_context io_context(1);

        asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&](auto, auto) { io_context.stop(); });

        auto in = Channel<int>::Make(io_context);
        auto head = in;
        auto out = Channel<int>::Make(io_context);

        // 开启一个协程做初始输入动作并监控每秒传递的次数
        co_spawn(io_context, [=]() {
            return start(in);
        }, asio::detached);

        for (int i = 0; i < MaxNum; i++)
        {
            co_spawn(io_context, [=]() {
                return foo(in, out);
            }, asio::detached);

            in = out;
            out = Channel<int>::Make(io_context);
        }
        co_spawn(io_context, [=]() {
            return foo(in, head);
        }, asio::detached);

        io_context.run();
    }
    catch (std::exception & e)
    {
        std::printf("Exception: %s\n", e.what());
    }
}
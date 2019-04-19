#include <iostream>
#include <asio.hpp>

#include "CoroutineRpc.h"
#include "echo_service.pb.h"
#include "echo_service.gayrpc.h"

using namespace coco;
using namespace dodo::test;

static
awaitable<void>
RpcTimeoutControl(Channel<std::pair<dodo::test::EchoResponse, gayrpc::core::RpcError>> waiter,
    std::chrono::steady_clock::duration timeout)
{
    // 随眠超时时间之后通过channel唤起RPC调用
    co_await Utils::Sleep(waiter.context(), timeout);
    dodo::test::EchoResponse response;
    waiter << std::make_pair(response, gayrpc::core::RpcError(true, 0, "timeout"));
}

static
awaitable<std::pair<dodo::test::EchoResponse, gayrpc::core::RpcError>>
MySyncEcho(dodo::test::EchoServerClient::PTR client,
    asio::io_context& context,
    const dodo::test::EchoRequest& request,
    std::chrono::steady_clock::duration timeout)
{
    Channel<std::pair<dodo::test::EchoResponse, gayrpc::core::RpcError>> waiter = Channel<std::pair<dodo::test::EchoResponse, 
        gayrpc::core::RpcError>>::Make(context);

    // 开启一个协程做超时检测
    co_spawn(context, [=]() {
        return RpcTimeoutControl(waiter, timeout);
    }, detached);
    // 发起异步RPC调用
    client->Echo(request, [waiter](const dodo::test::EchoResponse & response,
        const gayrpc::core::RpcError & error) mutable {
        waiter << std::make_pair(response, gayrpc::core::RpcError());
    });
    // 同步等待Response
    co_return co_await waiter.takeOne();
}

static std::atomic<uint32_t> n = 0;

static awaitable<void> benchmark(dodo::test::EchoServerClient::PTR client, asio::io_context& context)
{
    dodo::test::EchoRequest request;
    request.set_message("hello");
    for (;;)
    {
        auto [response, err] = co_await MySyncEcho(client, context, request, std::chrono::seconds(2));
        if (!err.failed())
        {
            n++;
        }
    }
}

static awaitable<void> output(asio::io_context& context)
{
    for (;;)
    {
        co_await Utils::Sleep(context, std::chrono::seconds(1));
        std::cout << "num:" << n << std::endl;
        n = 0;
    }
}

static awaitable<void> testRpc(asio::io_context& context, std::string ip, int port, int concurrentNum)
{
    tcp::socket socket(context);
    co_await socket.async_connect(asio::ip::tcp::endpoint(asio::ip::address::from_string(ip), 55555), asio::use_awaitable);

    auto serviceContext = InitRpcServiceContext(
        context,
        std::move(socket),
        gayrpc::utils::withProtectedCall(),
        gayrpc::utils::withProtectedCall());

    // 注册RPC客户端
    auto client = dodo::test::EchoServerClient::Create(serviceContext.getTypeHandleManager(),
        serviceContext.getInInterceptor(),
        serviceContext.getOutInterceptor());

    for (size_t i = 0; i < concurrentNum; i++)
    {
        co_spawn(context, [client, &context]() {
            return benchmark(client, context);
        }, detached);
    }
}

int main(int argc, char** argv)
{
    std::cout << " [work threadnum] - [ip] - [port] - [client num] - [concurrent  num]";

    try
    {
        asio::io_context io_context(std::atoi(argv[1]));

        asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&](auto, auto) { io_context.stop(); });

        for (size_t i = 0; i < std::atoi(argv[4]); i++)
        {
            co_spawn(io_context, [&]() {
                return testRpc(io_context, argv[2], std::atoi(argv[3]), std::atoi(argv[5]));
            }, detached);
        }

        co_spawn(io_context, [&]() {
            return output(io_context);
        }, detached);

        for (size_t i = 0; i < std::atoi(argv[1]); i++)
        {
            std::thread([&io_context]() {
                io_context.run();
            }).detach();
        }

        std::cin.get();
    }
    catch (std::exception & e)
    {
        std::printf("Exception: %s\n", e.what());
    }
}
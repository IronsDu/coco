#include <iostream>
#include <asio.hpp>
#include "CoroutineRpc.h"
#include "echo_service.pb.h"
#include "echo_service.gayrpc.h"

using namespace coco;
using namespace dodo::test;

static awaitable<void> coReply(const EchoServerService::EchoReply::PTR& replyObj, dodo::test::EchoResponse response)
{
    InterceptorContextType context;
    replyObj->reply(response, std::move(context));
    co_return;
}

class MyService : public dodo::test::EchoServerService
{
public:
    MyService(gayrpc::core::ServiceContext context,
        asio::io_context& ioContext)
        :
        EchoServerService(context),
        mContext(ioContext)
    {
    }

    void Echo(const EchoRequest& request,
        const EchoReply::PTR& replyObj,
        InterceptorContextType context) override
    {
		if (true)
		{
			co_spawn(mContext, [=]() {
				dodo::test::EchoResponse response;
				response.set_message("world");
				// 返回response
				return coReply(replyObj, response);
			}, detached);
		}
		else
		{
			dodo::test::EchoResponse response;
			response.set_message("world");
			InterceptorContextType context;
			replyObj->reply(response, std::move(context));
		}
    }

    void Login(const LoginRequest& request,
        const LoginReply::PTR& replyObj,
        InterceptorContextType context) override
    {
        LoginResponse response;
        response.set_message(request.message());
        replyObj->reply(response, std::move(context));
    }

private:
    asio::io_context& mContext;
};

awaitable<void> listener(asio::io_context& context, std::string ip, int port)
{
    tcp::acceptor acceptor(context);
    acceptor.open(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0).protocol());
    acceptor.bind(asio::ip::tcp::endpoint(asio::ip::address_v4::from_string(ip), port));
    acceptor.listen();

    for (;;)
    {
        tcp::socket socket = co_await acceptor.async_accept(use_awaitable);
        auto serviceContext = InitRpcServiceContext(
            context,
            std::move(socket),
            gayrpc::utils::withProtectedCall(),
            gayrpc::utils::withProtectedCall());
        // 注册RPC服务
        auto service = std::make_shared<MyService>(serviceContext, context);
        EchoServerService::Install(service);
    }
}

int main(int argc, char** argv)
{
    std::cout << " [work threadnum] - [ip] - [port]";

    try
    {
        asio::io_context io_context(std::atoi(argv[1]));

        asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&](auto, auto) { io_context.stop(); });

        co_spawn(io_context,
            [&]() {
                return listener(io_context, argv[2], std::atoi(argv[3]));
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
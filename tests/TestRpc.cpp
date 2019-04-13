#include <iostream>
#include <asio.hpp>
#include <brynet/utils/packet.h>

#include "coco/Channel.h"
#include "coco/Utils.h"
#include "coco/CoroutineMutex.h"
#include "coco/SharedSocket.h"
#include "coco/StreamReceive.h"

#include "pb/echo_service.pb.h"
#include "pb/echo_service.gayrpc.h"

#include "gayrpc/utils/UtilsInterceptor.h"
#include "gayrpc/protocol/BinaryProtocol.h"
#include "gayrpc/core/gayrpc_meta.pb.h"
#include "gayrpc/core/GayRpcTypeHandler.h"

using namespace coco;

class RpcSendMsg final
{
public:
    using Ptr = std::shared_ptr< RpcSendMsg>;

    RpcSendMsg()
    {
    }

    std::string meta;
    std::string data;
};

static awaitable<void> writer(SharedSocket::Ptr socket, Channel<RpcSendMsg::Ptr> msgChannel)
{
    brynet::utils::AutoMallocPacket<4096> buffer(true, true);
    for (;;)
    {
        buffer.init();
        auto msgList = co_await msgChannel.takeAll();
        while (!msgList.empty())
        {
            auto msg = msgList.front();
            msgList.pop();
            gayrpc::protocol::binary::serializeProtobufPacket(buffer, msg->meta, msg->data);
        }

        co_await asio::async_write(socket->socket(), asio::buffer(buffer.getData(), buffer.getPos()), asio::use_awaitable);
    }
}

static awaitable<void> reader(SharedSocket::Ptr socket, gayrpc::core::RpcTypeHandleManager::PTR rpcTypeHandler)
{
    StreamReceive stream(socket, 30);
    for (;;)
    {
        auto totalLen = co_await stream.asyncReadInt<uint64_t>(EndianType::Big);
        auto op = co_await stream.asyncReadInt<uint32_t>(EndianType::Big);
        assert(op == gayrpc::protocol::binary::OpCodeProtobuf);

        auto totalData = co_await stream.asyncRead(totalLen);

        brynet::utils::BasePacketReader bpr(totalData.data(), totalData.size());
        auto metaLen = bpr.readUINT32();
        auto dataLen = bpr.readUINT64();

        std::string_view meta = std::string_view(bpr.getBuffer() + bpr.getPos(), metaLen);
        bpr.addPos(metaLen);
        std::string_view data = std::string_view(bpr.getBuffer() + bpr.getPos(), dataLen);

        gayrpc::core::RpcMeta rpcMeta;
        auto success = rpcMeta.ParseFromArray(meta.data(), meta.size());
        assert(success);

        gayrpc::core::InterceptorContextType context;
        try
        {
            rpcTypeHandler->handleRpcMsg(rpcMeta, data, context);
        }
        catch (...)
        {
        }
    }
}

// asio同步RPC的消息发送拦截器
static auto withAsioBinarySender(Channel< RpcSendMsg::Ptr> sendMsgChannel)
{
    return [=](const gayrpc::core::RpcMeta & meta,
        const google::protobuf::Message & message,
        const gayrpc::core::UnaryHandler & next,
        gayrpc::core::InterceptorContextType context) mutable {
            RpcSendMsg::Ptr msg = std::make_shared<RpcSendMsg>();
            msg->meta = meta.SerializeAsString();
            msg->data = message.SerializeAsString();
            sendMsgChannel << msg;
            next(meta, message, std::move(context));
        };
}

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
        // 开一个协程
        co_spawn(mContext, [=]() {
            dodo::test::EchoResponse response;
            response.set_message("world");
            // 返回response
            return coReply(replyObj, response);
        }, detached);
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

gayrpc::core::ServiceContext InitRpcServiceContext(asio::io_context& context,
    tcp::socket socket,
    gayrpc::core::UnaryServerInterceptor inboundInterceptor,
    gayrpc::core::UnaryServerInterceptor outBoundInterceptor)
{
    auto socketPtr = SharedSocket::Make(std::move(socket));

    Channel< RpcSendMsg::Ptr> sendMsgChannel = Channel< RpcSendMsg::Ptr>::Make(context);
    auto rpcHandlerManager = std::make_shared<gayrpc::core::RpcTypeHandleManager>();

    // 开启写线程
    co_spawn(context, [=]() {
        return writer(socketPtr, sendMsgChannel);
    }, detached);

    // 开启读线程
    co_spawn(context, [=]() {
        return reader(socketPtr, rpcHandlerManager);
    }, detached);

    outBoundInterceptor = gayrpc::utils::makeInterceptor(outBoundInterceptor, withAsioBinarySender(sendMsgChannel));

    return ServiceContext(rpcHandlerManager, inboundInterceptor, outBoundInterceptor);
}

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
    Channel<std::pair<dodo::test::EchoResponse, gayrpc::core::RpcError>> waiter = Channel<std::pair<dodo::test::EchoResponse, gayrpc::core::RpcError>>::Make(context);

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

static awaitable<void> testRpc(asio::io_context& context)
{
    tcp::socket socket(context);
    co_await socket.async_connect(asio::ip::tcp::endpoint(asio::ip::address::from_string("127.0.0.1"), 55555), asio::use_awaitable);

    auto serviceContext = InitRpcServiceContext(
        context,
        std::move(socket),
        gayrpc::utils::withProtectedCall(),
        gayrpc::utils::withProtectedCall());

    // 注册RPC客户端
    auto client = dodo::test::EchoServerClient::Create(serviceContext.getTypeHandleManager(),
        serviceContext.getInInterceptor(),
        serviceContext.getOutInterceptor());

    // 注册RPC服务
    auto service = std::make_shared<MyService>(serviceContext, context);
    EchoServerService::Install(service);

    // 发起RPC请求
    dodo::test::EchoRequest request;
    request.set_message("hello");
    if (false) {
        client->Echo(request, [](const dodo::test::EchoResponse & response, const gayrpc::core::RpcError&) {
            std::cout << response.message() << std::endl;
        });
    }
    else
    {
        for (size_t i = 0; i < 100; i++)
        {
            auto [response, err] = co_await MySyncEcho(client, context, request, std::chrono::seconds(2));
            std::cout << response.message() << std::endl;
        }
    }
}

awaitable<void> listener(asio::io_context& context)
{
    auto executor = co_await this_coro::executor;
    tcp::acceptor acceptor(executor, { tcp::v4(), 55555 });
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

int main()
{
    try
    {
        asio::io_context io_context(1);

        asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&](auto, auto) { io_context.stop(); });

        co_spawn(io_context,
            [&]() {
                return listener(io_context);
            }, detached);

        co_spawn(io_context, [&]() {
                return testRpc(io_context);
            }, detached);

        io_context.run();
    }
    catch (std::exception & e)
    {
        std::printf("Exception: %s\n", e.what());
    }
}
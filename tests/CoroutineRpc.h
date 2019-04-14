#pragma once

#include <asio.hpp>
#include <brynet/utils/packet.h>

#include "coco/Channel.h"
#include "coco/Utils.h"
#include "coco/CoroutineMutex.h"
#include "coco/SharedSocket.h"
#include "coco/StreamReceive.h"

#include "gayrpc/utils/UtilsInterceptor.h"
#include "gayrpc/protocol/BinaryProtocol.h"
#include "gayrpc/core/gayrpc_meta.pb.h"
#include "gayrpc/core/GayRpcTypeHandler.h"
#include "gayrpc/core/GayRpcService.h"

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
    StreamReceive stream(socket, 1024*1024);
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

static gayrpc::core::ServiceContext InitRpcServiceContext(asio::io_context& context,
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

    return gayrpc::core::ServiceContext(rpcHandlerManager, inboundInterceptor, outBoundInterceptor);
}
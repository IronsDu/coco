#pragma once

#include <memory>
#include <asio/ip/tcp.hpp>
#include <asio/io_context.hpp>

namespace coco {

    using namespace asio::ip;

    class SharedSocket
    {
    public:
        using Ptr = std::shared_ptr<SharedSocket>;

        static SharedSocket::Ptr Make(tcp::socket socket)
        {
            return std::make_shared<SharedSocket>(std::move(socket));
        }

        SharedSocket(tcp::socket socket)
            :
            mSocket(std::move(socket))
        {}

        virtual ~SharedSocket() = default;

        tcp::socket& socket()
        {
            return mSocket;
        }

    private:
        tcp::socket mSocket;
    };
}
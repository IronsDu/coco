#pragma once

#include <chrono>

#include <asio/io_context.hpp>
#include <asio/awaitable.hpp>
#include <asio/steady_timer.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>

namespace coco {

    class Utils
    {
    public:
        static asio::awaitable<void> Sleep(asio::io_context& context, std::chrono::steady_clock::duration timeout)
        {
            asio::steady_timer timer(context);
            timer.expires_from_now(timeout);
            asio::error_code ec;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        }
    };

}

#pragma once

#include <memory>
#include <atomic>
#include <cassert>

#include <asio/detail/noncopyable.hpp>
#include <asio/io_context.hpp>
#include <asio/awaitable.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/steady_timer.hpp>

namespace coco {

    using namespace asio;
    using asio::redirect_error;
    using asio::use_awaitable;

    //TODO::support RAII

    class CoroutineMutex : public asio::noncopyable
    {
    public:
        using Ptr = std::shared_ptr<CoroutineMutex>;

        static Ptr Make(asio::io_context& context)
        {
            struct make_shared_enabler : public CoroutineMutex
            {
                make_shared_enabler(asio::io_context& context)
                    :
                    CoroutineMutex(context)
                {}
            };
            return std::make_shared<make_shared_enabler>(context);
        }

        asio::io_context& context()
        {
            return mContext;
        }

        awaitable<void> lock()
        {
            asio::error_code ec;
            while (mLocked.exchange(true))
            {
                co_await mTimer.async_wait(redirect_error(use_awaitable, ec));
            }
        }

        void unlock()
        {
            auto o = mLocked.exchange(false);
            assert(o);
            mTimer.cancel_one();
        }

    private:
        CoroutineMutex(asio::io_context& context)
            :
            mLocked(false),
            mContext(context),
            mTimer(context)
        {
        }

    protected:
        virtual ~CoroutineMutex() = default;

    private:
        std::atomic_bool    mLocked;
        asio::io_context&   mContext;
        asio::steady_timer  mTimer;
    };
}
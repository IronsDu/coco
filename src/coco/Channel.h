#pragma once

#include <queue>
#include <mutex>

#include <asio/steady_timer.hpp>
#include <asio/io_context.hpp>
#include <asio/awaitable.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>

namespace coco {

    using asio::awaitable;
    using asio::use_awaitable;
    using asio::redirect_error;

    //TODO::support timeout

    template<typename T>
    class Channel
    {
    private:
        template<typename T>
        class ChannelDetail : public asio::noncopyable
        {
        public:
            using Ptr = std::shared_ptr<ChannelDetail<T>>;
            using Container = std::queue<T>;

            static Ptr Make(asio::io_context& context)
            {
                struct make_shared_enabler : public ChannelDetail<T>
                {
                    make_shared_enabler(asio::io_context& context)
                        :
                        ChannelDetail<T>(context)
                    {}
                };
                return std::make_shared<make_shared_enabler>(context);
            }

        private:
            ChannelDetail(asio::io_context& context)
                :
                mContext(context),
                mTimer(context)
            {
                mTimer.expires_at(std::chrono::steady_clock::time_point::max());
            }

        protected:
            virtual ~ChannelDetail() = default;

        private:
            asio::io_context&   mContext;
            asio::steady_timer  mTimer;

            Container           mValues;
            std::mutex          mValuesGuard;

            friend class Channel<T>;
        };

    public:
        Channel(typename ChannelDetail<T>::Ptr ptr)
            :
            mPtr(ptr)
        {}

        static Channel Make(asio::io_context& context)
        {
            return Channel<T>(ChannelDetail<T>::Make(context));
        }

        auto& operator << (T value)
        {
            {
                std::lock_guard<std::mutex> lck(mPtr->mValuesGuard);
                mPtr->mValues.push(std::move(value));
            }
            mPtr->mTimer.cancel_one();

            return *this;
        }

        asio::io_context& context()
        {
            return mPtr->mContext;
        }

        asio::awaitable<T> takeOne()
        {
            while (true)
            {
                {
                    std::lock_guard<std::mutex> lck(mPtr->mValuesGuard);
                    if (!mPtr->mValues.empty())
                    {
                        auto result = mPtr->mValues.front();
                        mPtr->mValues.pop();
                        co_return result;
                    }
                }

                asio::error_code ec;
                co_await mPtr->mTimer.async_wait(redirect_error(use_awaitable, ec));
            }
        }

        asio::awaitable<typename ChannelDetail<T>::Container>  takeAll()
        {
            while (true)
            {
                {
                    std::lock_guard<std::mutex> lck(mPtr->mValuesGuard);
                    if (!mPtr->mValues.empty())
                    {
                        co_return std::move(mPtr->mValues);
                    }
                }

                asio::error_code ec;
                co_await mPtr->mTimer.async_wait(redirect_error(use_awaitable, ec));
            }
        }

        virtual ~Channel() = default;

    private:
        typename ChannelDetail<T>::Ptr mPtr;
    };

}
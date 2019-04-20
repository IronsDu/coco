#pragma once

#include <string>
#include <string_view>
#include <coco/SharedSocket.h>

namespace coco {

    using asio::awaitable;

    enum class EndianType
    {
        Big,
        Small,
    };

    class StreamReceive
    {
    public:
        StreamReceive(SharedSocket::Ptr socketPtr, size_t maxBufferSize)
            :
            mMaxBufferSize(maxBufferSize),
            mSocketPtr(socketPtr),
            mValidStartPos(0),
            mValidEndPos(0)
        {
            growBuffer();
        }

        virtual ~StreamReceive() = default;

        awaitable<std::string_view> asyncRead(size_t len)
        {
            while (true)
            {
                if (getValidSize() >= len)
                {
                    const auto startValidData = getValidStartData();
                    mValidStartPos += len;
                    co_return std::string_view(startValidData, len);
                }
                co_await receiveMore();
            }
        }

        awaitable<std::string_view> asyncReadUtil(std::string expected)
        {
            while (true)
            {
                const auto findPos = std::string_view(getValidStartData(), getValidSize()).find(expected);
                if (findPos != std::string::npos)
                {
                    const auto startValidData = getValidStartData();
                    mValidStartPos += findPos;
                    co_return std::string_view(startValidData, findPos);
                }
                co_await receiveMore();
            }
        }

        template<typename IntType>
        awaitable<IntType> asyncReadInt(EndianType endian)
        {
            static_assert(
                std::is_same<std::int8_t, IntType>::value ||
                std::is_same<std::int16_t, IntType>::value ||
                std::is_same<std::int32_t, IntType>::value ||
                std::is_same<std::int64_t, IntType>::value ||
                std::is_same<std::uint8_t, IntType>::value ||
                std::is_same<std::uint16_t, IntType>::value ||
                std::is_same<std::uint32_t, IntType>::value ||
                std::is_same<std::uint64_t, IntType>::value, "");
            static_assert(sizeof(std::string::value_type) == sizeof(int8_t), "");

            auto data = co_await asyncRead(sizeof(IntType));
            assert(data.size() == sizeof(IntType));
            switch (endian)
            {
            case EndianType::Big:
            {
                //TODO::optimize
                std::string tmp(data.data(), data.size());
                std::reverse(tmp.begin(), tmp.end());
                co_return *(IntType*)(tmp.data());
            }
            default:
                co_return *(IntType*)(data.data());
            }
        }

    private:
        asio::awaitable<void>  receiveMore()
        {
            if (getValidSize() >= mMaxBufferSize)
            {
                throw std::runtime_error("buffer is full");
            }
            else if ((getValidSize() == 0) || (mRemainder.size() == mValidEndPos))
            {
                assert(getValidSize() <= mRemainder.size());
                memcpy(mRemainder.data(), getValidStartData(), getValidSize());
                mValidEndPos = getValidSize();
                mValidStartPos = 0;
            }

            const std::size_t n = co_await mSocketPtr->socket().async_read_some(
                asio::buffer(getValidEndData(), mRemainder.size() - mValidEndPos),
                asio::use_awaitable);
            mValidEndPos += n;

            if (getValidSize() == mRemainder.size())
            {
                growBuffer();
            }
        }

        size_t      getValidSize() const
        {
            return (mValidEndPos - mValidStartPos);
        }

        const char* getValidStartData() const
        {
            return (mRemainder.data() + mValidStartPos);
        }

        char*       getValidEndData()
        {
            return (mRemainder.data() + mValidEndPos);
        }

        void        growBuffer()
        {
            const auto GrowSize = 1024;
            mRemainder.resize(std::min(mRemainder.size() + GrowSize, mMaxBufferSize));
        }

    private:
        const size_t        mMaxBufferSize;
        SharedSocket::Ptr   mSocketPtr;
        std::string         mRemainder;
        size_t              mValidStartPos;
        size_t              mValidEndPos;
    };

}
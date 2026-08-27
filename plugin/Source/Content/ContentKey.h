#pragma once

#include <cstdint>
#include <functional>

// 对应 OpenTune Source/Content/ContentKey.h（我们只有 ARAAudioModification 一个域）
namespace deepsvc
{

enum class DomainKind : uint8_t
{
    ARAAudioModification
};

// 标识一个内容根（一个 ARA AudioModification）。不携带任何指针。
struct ContentKey
{
    DomainKind domainKind { DomainKind::ARAAudioModification };
    uint64_t objectId { 0 };  // DocumentController 分配的持久对象 ID

    bool isValid() const noexcept { return objectId != 0; }
    bool operator== (const ContentKey& rhs) const noexcept
    {
        return domainKind == rhs.domainKind && objectId == rhs.objectId;
    }
    bool operator!= (const ContentKey& rhs) const noexcept { return ! (*this == rhs); }
    bool operator< (const ContentKey& rhs) const noexcept
    {
        if (domainKind != rhs.domainKind)
            return domainKind < rhs.domainKind;
        return objectId < rhs.objectId;
    }
};

// 标识一个分段：内容根 + 分段区间起点（修改内部时间，微秒取整）。
// 分段区间随归档持久，起点在同一个修改内唯一（分段互不重叠）
struct SegmentKey
{
    ContentKey content;
    int64_t startMicros { 0 };

    bool isValid() const noexcept { return content.isValid(); }
    bool operator== (const SegmentKey& rhs) const noexcept
    {
        return content == rhs.content && startMicros == rhs.startMicros;
    }
    bool operator!= (const SegmentKey& rhs) const noexcept { return ! (*this == rhs); }
    bool operator< (const SegmentKey& rhs) const noexcept
    {
        if (content != rhs.content)
            return content < rhs.content;
        return startMicros < rhs.startMicros;
    }

    static int64_t microsFromSeconds (double seconds) noexcept
    {
        return static_cast<int64_t> (seconds * 1.0e6 + (seconds >= 0.0 ? 0.5 : -0.5));
    }
};

} // namespace deepsvc

namespace std
{
template <>
struct hash<deepsvc::ContentKey>
{
    size_t operator() (const deepsvc::ContentKey& key) const noexcept
    {
        size_t h = std::hash<uint8_t> {} (static_cast<uint8_t> (key.domainKind));
        h ^= std::hash<uint64_t> {} (key.objectId) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

template <>
struct hash<deepsvc::SegmentKey>
{
    size_t operator() (const deepsvc::SegmentKey& key) const noexcept
    {
        size_t h = std::hash<deepsvc::ContentKey> {} (key.content);
        h ^= std::hash<int64_t> {} (key.startMicros) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};
} // namespace std

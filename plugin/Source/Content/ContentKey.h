#pragma once

#include <cstdint>
#include <functional>

namespace deepsvc
{

enum class DomainKind : uint8_t
{
    ARAAudioModification
};

// 标识一份 AudioModification 可编辑内容。由 persistentID 的哈希生成，不携带指针。
struct ContentKey
{
    DomainKind domainKind { DomainKind::ARAAudioModification };
    uint64_t objectId { 0 };

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
} // namespace std

// libstdc++ functions a Tactility device app needs and Tactility does not
// export to apps (see TactilityLibc.c for why they are defined in the app).
//
// These are the out-of-line parts of std::unordered_map/set, std::list and
// std::make_shared that live in libstdc++.so rather than in its headers. Every
// caller is inside the app, so they only have to agree with each other, not
// with the firmware's copies: the hash need not be libstdc++'s, and the bucket
// counts need only be primes the policy then keeps to.

#if defined(DEKI_TACTILITY_TARGET) && defined(ESP_PLATFORM) && !defined(DEKI_EDITOR)

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <typeinfo>
#include <unordered_map>
#include <utility>

namespace
{

bool IsPrime(std::size_t n)
{
    if (n < 2)
    {
        return false;
    }
    if (n % 2 == 0)
    {
        return n == 2;
    }
    for (std::size_t d = 3; d <= n / d; d += 2)
    {
        if (n % d == 0)
        {
            return false;
        }
    }
    return true;
}

std::size_t NextPrime(std::size_t n)
{
    if (n <= 2)
    {
        return 2;
    }
    if (n % 2 == 0)
    {
        ++n;
    }
    while (!IsPrime(n))
    {
        if (n > SIZE_MAX - 2)
        {
            return SIZE_MAX;
        }
        n += 2;
    }
    return n;
}

}  // namespace

namespace std
{

// MurmurHash2 (Austin Appleby, public domain), the 32-bit hash libstdc++
// itself uses where size_t is 32 bits.
size_t _Hash_bytes(const void* ptr, size_t len, size_t seed)
{
    const uint32_t m = 0x5bd1e995;
    const int r = 24;
    uint32_t h = static_cast<uint32_t>(seed) ^ static_cast<uint32_t>(len);
    const auto* data = static_cast<const unsigned char*>(ptr);

    while (len >= 4)
    {
        uint32_t k;
        std::memcpy(&k, data, sizeof(k));
        k *= m;
        k ^= k >> r;
        k *= m;
        h *= m;
        h ^= k;
        data += 4;
        len -= 4;
    }
    switch (len)
    {
        case 3: h ^= static_cast<uint32_t>(data[2]) << 16; [[fallthrough]];
        case 2: h ^= static_cast<uint32_t>(data[1]) << 8; [[fallthrough]];
        case 1: h ^= data[0]; h *= m;
    }
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;
    return h;
}

namespace __detail
{

// The smallest prime bucket count of at least n, and the element count at
// which the table must grow next.
size_t _Prime_rehash_policy::_M_next_bkt(size_t n) const
{
    const size_t buckets = n == 0 ? 1 : NextPrime(n);
    const float resize = static_cast<float>(buckets) * _M_max_load_factor;
    _M_next_resize = resize >= static_cast<float>(SIZE_MAX) ? SIZE_MAX : static_cast<size_t>(resize);
    return buckets;
}

// Whether inserting n_ins elements into a table of n_bkt buckets holding
// n_elt must grow it, and to how many buckets. An empty table starts with
// room for 11 elements.
pair<bool, size_t> _Prime_rehash_policy::_M_need_rehash(size_t n_bkt, size_t n_elt, size_t n_ins) const
{
    if (n_elt + n_ins <= _M_next_resize)
    {
        return { false, 0 };
    }
    size_t wanted = n_elt + n_ins;
    if (_M_next_resize == 0 && wanted < 11)
    {
        wanted = 11;
    }
    const float min_bkts = static_cast<float>(wanted) / _M_max_load_factor;
    if (min_bkts >= static_cast<float>(n_bkt))
    {
        const size_t grown = n_bkt * _S_growth_factor;
        const size_t needed = static_cast<size_t>(min_bkts) + 1;
        return { true, _M_next_bkt(needed > grown ? needed : grown) };
    }
    _M_next_resize = static_cast<size_t>(static_cast<float>(n_bkt) * _M_max_load_factor);
    return { false, 0 };
}

// A std::list node's links. A list is a ring through its sentinel node.

// Puts this node in front of `position`.
void _List_node_base::_M_hook(_List_node_base* const position) noexcept
{
    _M_next = position;
    _M_prev = position->_M_prev;
    position->_M_prev->_M_next = this;
    position->_M_prev = this;
}

// Takes this node out of its list; its own links are left as they were.
void _List_node_base::_M_unhook() noexcept
{
    _M_prev->_M_next = _M_next;
    _M_next->_M_prev = _M_prev;
}

// Moves the nodes [first, last) in front of this one (splice). They may come
// from this list or another; nothing moves when this node is `last`.
void _List_node_base::_M_transfer(_List_node_base* const first, _List_node_base* const last) noexcept
{
    if (this == last)
    {
        return;
    }
    _List_node_base* const lastNode = last->_M_prev;
    _List_node_base* const before = first->_M_prev;

    // Close the gap they leave.
    before->_M_next = last;
    last->_M_prev = before;

    // And link them in between this node and the one before it.
    _List_node_base* const previous = _M_prev;
    previous->_M_next = first;
    first->_M_prev = previous;
    lastNode->_M_next = this;
    _M_prev = lastNode;
}

}  // namespace __detail

// Without RTTI, make_shared marks its control block with the address of a
// private tag object (_S_ti) instead of a real type_info.
bool _Sp_make_shared_tag::_S_eq(const type_info& ti) noexcept
{
    return &ti == &_S_ti();
}

}  // namespace std

#endif

#pragma once
#include <algorithm>

// Extra subscribers do not replace an explicitly selected device. Reject a
// missing or ambiguous match; never silently choose the first available peer.
template<class Range, class Predicate>
auto UniqueSubscriber(const Range& peers, Predicate matches) {
    auto selected = std::find_if(peers.begin(), peers.end(), matches);
    if (selected == peers.end()) return selected;
    auto next = selected;
    if (std::find_if(++next, peers.end(), matches) != peers.end()) return peers.end();
    return selected;
}

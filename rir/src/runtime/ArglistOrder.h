#ifndef ARGLIST_ORDER_H
#define ARGLIST_ORDER_H

#include "RirRuntimeObject.h"

#include <vector>

namespace rir {

#pragma pack(push)
#pragma pack(1)

constexpr static size_t ARGLIST_ORDER_MAGIC = 0xaabbccdd;

struct Code;

struct ArglistOrder
    : public RirRuntimeObject<ArglistOrder, ARGLIST_ORDER_MAGIC> {

    using CallId = size_t;
    using ArgIdx = uint8_t;
    using CallArglistOrder = std::vector<ArgIdx>;

    static constexpr ArgIdx ARG_NAMED_MASK = 1L << ((8 * sizeof(ArgIdx)) - 1);
    static constexpr CallId ORIGINAL_ARGLIST_ORDER = -1;

    static ArgIdx encodeArg(ArgIdx val, bool named) {
        return named ? val | ARG_NAMED_MASK : val;
    }

    static ArgIdx decodeArg(ArgIdx val) { return val & ~ARG_NAMED_MASK; }

    static bool isArgNamed(ArgIdx val) { return val & ARG_NAMED_MASK; }

    static size_t size(std::vector<CallArglistOrder> const& reordering) {
        size_t sz = 0;
        for (auto const& r : reordering)
            sz += r.size();
        return sizeof(ArglistOrder) + 2 * reordering.size() + sz;
    }

    ArglistOrder(std::vector<CallArglistOrder> const& reordering)
        : RirRuntimeObject(0, 0), nCalls(reordering.size()) {
        auto offset = nCalls * 2;
        for (size_t i = 0; i < nCalls; i++) {
            data[2 * i] = offset;
            auto n = reordering[i].size();
            data[2 * i + 1] = n;
            offset += n;
            memcpy(data + offset, reordering[i].data(), n);
        }
    }

    ArgIdx index(CallId callId, size_t i) const {
        assert(callId < nCalls);
        auto offset = data[callId * 2];
        auto length = data[callId * 2 + 1];
        assert(i < length);
        return data[offset + i];
    }

    size_t nCalls;
    ArgIdx data[];

    /* Layout of data is nCalls * (offset, length), followed by nCalls *
    variable length list of indices

       [
    0    (6, 3),
    2    (9, 2),
    4    (11, 4),
    6    [0, 1, 2],
    9    [0, 1],
    11   [0, 1, 2, 3]
       ]

    0: a(), 3
    1: b(), 2
    2: c(), 4

    */
};

#pragma pack(pop)

} // namespace rir

#endif

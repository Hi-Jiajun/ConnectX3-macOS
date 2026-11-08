/* Copyright (c) 2026 Hi-Jiajun and contributors. OpenIB.org BSD; see LICENSE.
 * A0: SET_PORT qpn_calc maps each MAC slot to base + index. Reserve a full
 * disjoint MAC window per port, not only its currently populated slot zero.
 * See vendor/freebsd-src/mlx4_core/mlx4_port.c:372-375,1578-1600.
 * These are driver allocations within the submitted INIT_HCA profile, not a
 * claim that QUERY_DEV_CAP's FW-reserved count is an Ethernet steering base. */
#ifndef CX3_QUEUE_NAMESPACE_H
#define CX3_QUEUE_NAMESPACE_H
#include "cx3_abi.h"
struct Cx3QueueNumbers {
    uint32_t rxQpn, rssQpn, txQpn, rxCqn, txCqn, macBase, macSlots;
};
struct Cx3QueueNamespace {
    bool valid;
    uint64_t generation;
    uint32_t ports, numQps, numCqs, maxMacLog;
    Cx3QueueNumbers numbers[2];
    void init() { valid=false; generation=0; ports=numQps=numCqs=maxMacLog=0; }
    bool configure(uint32_t qpLog, uint32_t cqLog, uint32_t reservedQps,
                   uint32_t reservedCqs, uint32_t portCount, uint32_t macLog, uint64_t gen)
    {
        if (valid || !gen || portCount<1 || portCount>2 || qpLog>24 || cqLog>24 || macLog>7)
            return false;
        const uint32_t nq=1u<<qpLog, nc=1u<<cqLog, slots=1u<<macLog;
        const uint32_t floor=reservedQps<2 ? 2 : reservedQps;
        const uint32_t cqFloor=reservedCqs<1 ? 1 : reservedCqs;
        const uint64_t windows=uint64_t(slots)*portCount;
        if (windows>nq || uint64_t(floor)+2u*portCount>nq-windows ||
            uint64_t(cqFloor)+2u*portCount>nc) return false;
        Cx3QueueNumbers n[2]{};
        for (uint32_t i=0;i<portCount;++i) {
            n[i].macBase=nq-uint32_t(windows)+i*slots;
            n[i].macSlots=slots; n[i].rxQpn=n[i].macBase;
            n[i].rssQpn=floor+2*i; n[i].txQpn=floor+2*i+1;
            n[i].rxCqn=cqFloor+2*i; n[i].txCqn=cqFloor+2*i+1;
        }
        for (uint32_t i=0;i<2;++i) numbers[i]=n[i];
        generation=gen; ports=portCount; numQps=nq; numCqs=nc; maxMacLog=macLog;
        valid=true; return true;
    }
};
#endif

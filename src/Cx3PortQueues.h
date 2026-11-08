/* Copyright (c) 2026 Hi-Jiajun and contributors. OpenIB.org BSD; see LICENSE.
 * Durable queue owners for one physical port. PF command/EQ/MR/ICM/MTT stay
 * shared. A zero Up bit never permits overwriting a retained pointer.
 * Never copy a live instance or reinitialize it to dispose of an owner. */
#ifndef CX3_PORT_QUEUES_H
#define CX3_PORT_QUEUES_H
#include "cx3_abi.h"
#include "Cx3QueueNamespace.h"
struct Mlx4Icm;
struct Cx3PortQueues {
    uint32_t port;
    bool numbersReserved, setupAttempted, setupFailed, setupReady;
    uint64_t numberGeneration, completedStages;
    uint32_t setupStage, failedStage, lastFwStatus;
    Cx3QueueNumbers numbers;
    bool cqUp;
    bool cqUnknownRetained;
    bool cqHwReturned;
    bool cqMayBeDeviceOwned;
    Mlx4Icm * cqRing;
    uint32_t cqNent;
    uint32_t cqCqn;
    uint32_t cqMttOff;
    uint32_t cqMttPages;
    Mlx4Icm * cqRec;
    uint64_t cqRecPhys;
    uint32_t cqConsIndex;
    uint32_t cqUarStart;
    bool rqUp;
    bool rqUnknownRetained;
    Mlx4Icm * rqRing;
    Mlx4Icm * rqBufs;
    uint32_t rqNent;
    uint32_t rqBufBytes;
    uint32_t rqPages;
    uint32_t rqMttOff;
    uint32_t rqMttPages;
    uint32_t rqMttAddr;
    uint32_t rqLkey;
    uint64_t rqBufPhys;
    uint32_t rqProd;
    bool qpUp;
    uint32_t qpQpn;
    uint32_t qpState;
    Mlx4Icm * qpRec;
    uint64_t qpRecPhys;
    uint32_t qpProd;
    bool macUp;
    uint32_t macSlot;
    uint8_t mac[8];
    uint64_t macValue;
    bool ingressUp;
    uint32_t ingressQpn;
    uint32_t ingressState;
    Mlx4Icm * ingressRec;
    uint64_t ingressRecPhys;
    uint32_t ingressProd;
    bool txCqUp;
    bool txCqHwReturned;
    uint32_t txCqn;
    Mlx4Icm * txCqRing;
    uint32_t txCqNent;
    uint32_t txCqMttOff;
    uint32_t txCqMttPages;
    Mlx4Icm * txCqRec;
    bool txQpUp;
    bool txQpReturned;
    uint32_t txQpn;
    uint32_t txQpState;
    Mlx4Icm * txSq;
    uint32_t txSqNent;
    uint32_t txSqPages;
    uint32_t txSqMttOff;
    uint32_t txSqMttPages;
    uint32_t txSqMttAddr;
    uint64_t txSqPhys;
    Mlx4Icm * txQpRec;
    bool txSqUnknownRetained;
    uint64_t txQpRecPhys;
    Mlx4Icm * rawFrame;
    uint64_t rawFramePhys;
    uint32_t rawFrameBytes;
    uint32_t rawTxProd;
    uint32_t rawTxCons;
    uint32_t rawRxProd;
    uint32_t rawRxCons;
    uint32_t rawRxRecyclesTotal;
    uint32_t rawCalls;
    uint32_t rawRxRecycles[CX3_R4RAW_MAX_RX_SLOTS];
    uint8_t rawRxInFlight[CX3_R4RAW_MAX_RX_SLOTS / 8];
    bool nativeTxCsum, nativeRxCsum, vlanFilter, vlanApplied;
    uint8_t vlanBits[512]; /* VID i -> byte i/8, bit i%8; reserved 4095 rejected */
    uint32_t vlanFwStatus;
    uint64_t vlanRevision;

    /* Only for an empty instance during controller init, before publication. */
    void init(uint32_t physicalPort)
    {
        port = physicalPort;
        nativeTxCsum=nativeRxCsum=true;vlanFilter=vlanApplied=false;
        vlanFwStatus=0xffffffffu;vlanRevision=0;
        for(unsigned i=0;i<sizeof(vlanBits);++i)vlanBits[i]=0;
        numbersReserved=setupAttempted=setupFailed=setupReady=false;
        numberGeneration=completedStages=0; setupStage=failedStage=0;
        lastFwStatus=0xffffffffu; numbers=Cx3QueueNumbers{};
        cqUp = 0;
        cqUnknownRetained = 0;
        cqHwReturned = 0;
        cqMayBeDeviceOwned = 0;
        cqRing = 0;
        cqNent = 0;
        cqCqn = 0;
        cqMttOff = 0;
        cqMttPages = 0;
        cqRec = 0;
        cqRecPhys = 0;
        cqConsIndex = 0;
        cqUarStart = 0;
        rqUp = 0;
        rqUnknownRetained = 0;
        rqRing = 0;
        rqBufs = 0;
        rqNent = 0;
        rqBufBytes = 0;
        rqPages = 0;
        rqMttOff = 0;
        rqMttPages = 0;
        rqMttAddr = 0;
        rqLkey = 0;
        rqBufPhys = 0;
        rqProd = 0;
        qpUp = 0;
        qpQpn = 0;
        qpState = 0;
        qpRec = 0;
        qpRecPhys = 0;
        qpProd = 0;
        macUp = 0;
        macSlot = 0;
        for (unsigned i = 0; i < sizeof(mac) / sizeof(mac[0]); ++i) mac[i] = 0;
        macValue = 0;
        ingressUp = 0;
        ingressQpn = 0;
        ingressState = 0;
        ingressRec = 0;
        ingressRecPhys = 0;
        ingressProd = 0;
        txCqUp = 0;
        txCqHwReturned = 0;
        txCqn = 0;
        txCqRing = 0;
        txCqNent = 0;
        txCqMttOff = 0;
        txCqMttPages = 0;
        txCqRec = 0;
        txQpUp = 0;
        txQpReturned = 0;
        txQpn = 0;
        txQpState = 0;
        txSq = 0;
        txSqNent = 0;
        txSqPages = 0;
        txSqMttOff = 0;
        txSqMttPages = 0;
        txSqMttAddr = 0;
        txSqPhys = 0;
        txQpRec = 0;
        txSqUnknownRetained = 0;
        txQpRecPhys = 0;
        rawFrame = 0;
        rawFramePhys = 0;
        rawFrameBytes = 0;
        rawTxProd = 0;
        rawTxCons = 0;
        rawRxProd = 0;
        rawRxCons = 0;
        rawRxRecyclesTotal = 0;
        rawCalls = 0;
        for (unsigned i = 0; i < sizeof(rawRxRecycles) / sizeof(rawRxRecycles[0]); ++i) rawRxRecycles[i] = 0;
        for (unsigned i = 0; i < sizeof(rawRxInFlight) / sizeof(rawRxInFlight[0]); ++i) rawRxInFlight[i] = 0;
    }
    bool hasOwners() const
    {
        return cqUp || cqUnknownRetained || cqMayBeDeviceOwned || cqRing || cqRec ||
            rqUp || rqUnknownRetained || rqRing || rqBufs || qpUp || qpRec ||
            macUp || ingressUp || ingressRec || txCqUp || txCqRing || txCqRec ||
            txQpUp || txQpRec || txSqUnknownRetained || txSq || rawFrame;
    }
};
#endif

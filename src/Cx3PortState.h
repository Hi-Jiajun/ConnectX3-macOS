/* Copyright (c) 2026 Hi-Jiajun and contributors. OpenIB.org BSD; see LICENSE.
 * Physical-port management state only. No DMA pointer, PF allocator or lock is
 * copied here. A QUERY/INIT/CLOSE receipt never grants release of a queue. */
#ifndef CX3_PORT_STATE_H
#define CX3_PORT_STATE_H
#include "cx3_abi.h"
struct Cx3PortState {
    uint32_t number;
    uint64_t generation, queryUptimeNs;
    bool queryValid, lastQueryFailed, macValid, linkKnown, linkUp;
    bool mtuConfigured, initAttempted, initAccepted, closeConfirmed;
    uint32_t speedMbps, frameMtu, maxPayloadMtu;
    uint32_t queryFwStatus, initFwStatus, closeFwStatus, mtuFwStatus;
    uint8_t mac[6], raw[CX3_QUERY_PORT_RAW_LEN];

    void init(uint32_t port, uint64_t gen)
    {
        *this = Cx3PortState{};
        number = port; generation = gen;
        queryFwStatus = initFwStatus = closeFwStatus = mtuFwStatus = 0xffffffffu;
    }
    void newGeneration(uint64_t gen)
    {
        /* Invalidating an observation is not proof that a prior INIT returned
         * ownership. Preserve knowledge that an open was attempted. */
        bool attempted = initAttempted;
        init(number, gen);
        initAttempted = attempted;
    }
    bool observe(const Cx3QueryPortResult &q, uint64_t gen, uint64_t uptimeNs)
    {
        if (q.port != number || gen != generation || !gen || !uptimeNs) return false;
        queryFwStatus = q.fwStatus;
        if (q.status || q.fwStatus || q.rawLen != CX3_QUERY_PORT_RAW_LEN) {
            lastQueryFailed = true; return false;
        }
        queryValid = linkKnown = true;
        lastQueryFailed = false;
        linkUp = q.linkState != 0;
        speedMbps = cx3LinkSpeedMbps(q.linkSpeed);
        maxPayloadMtu = q.ethMtu;
        queryUptimeNs = uptimeNs;
        bool anyMacByte = false;
        for (unsigned i = 0; i < 6; ++i) { mac[i] = q.defMac[i + 2]; anyMacByte |= mac[i] != 0; }
        macValid = anyMacByte && !(mac[0] & 1u);
        for (unsigned i = 0; i < sizeof(raw); ++i) raw[i] = q.raw[i];
        return true;
    }
    void snapshot(Cx3PortInfoResult &r) const
    {
        r = Cx3PortInfoResult{};
        r.version = CX3_PORT_INFO_VERSION; r.structBytes = sizeof(r);
        r.deviceGeneration = generation; r.port = number;
        r.flags = (queryValid ? CX3_PORT_QUERY_VALID : 0u) |
            (macValid ? CX3_PORT_MAC_VALID : 0u) | (linkKnown ? CX3_PORT_LINK_VALID : 0u) |
            (mtuConfigured ? CX3_PORT_MTU_CONFIGURED : 0u) |
            (initAttempted ? CX3_PORT_INIT_ATTEMPTED : 0u) |
            (initAccepted ? CX3_PORT_INIT_ACCEPTED : 0u) |
            (closeConfirmed ? CX3_PORT_CLOSE_CONFIRMED : 0u) |
            (lastQueryFailed ? CX3_PORT_LAST_QUERY_FAILED : 0u);
        r.queryFwStatus = queryFwStatus; r.initFwStatus = initFwStatus;
        r.closeFwStatus = closeFwStatus; r.mtuFwStatus = mtuFwStatus;
        r.frameMtu = frameMtu; r.maxPayloadMtu = maxPayloadMtu;
        r.linkUp = linkUp ? 1u : 0u; r.speedMbps = speedMbps;
        for (unsigned i = 0; i < 6; ++i) r.mac[i] = mac[i];
        r.queryUptimeNs = queryUptimeNs;
        if (queryValid) {
            r.rawBytes = sizeof(raw);
            for (unsigned i = 0; i < sizeof(raw); ++i) r.raw[i] = raw[i];
        }
    }
};
#endif

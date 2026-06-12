// SPDX-License-Identifier: LicenseRef-norxs-RI-1.0
// SPDX-FileCopyrightText: (c) 2026 norxs Technology LLC <contact@norxs.com>
//
/**
 * =====================================================================================
 * @file        zonal_gateway_demo.cpp
 * @brief       Self-contained, host-runnable integration demo of the norxs TSN stack.
 *
 *              Walks the full ASIL-D lifecycle against a simulated switch ASIC:
 *                1. Init()  — 8-phase bring-up with Network Calculus pre-validation
 *                2. Start() — kPtpConverging; BMCA self-elects this node grandmaster
 *                3. Tick()  — gPTP lock → GCL load on both ports → kRunning
 *                4. Fault injection — corrupt the hardware GCL → kDegraded
 *                5. Persistent fault → kSafeState (TC7-exclusive schedule applied)
 *                6. Audit-log dump — the ISO 26262 §7.4.2 forensic trail
 *
 *              Build:  cmake -B build -DNORXS_BUILD_EXAMPLES=ON
 *                      cmake --build build --target zonal-gateway-demo
 *              Run:    ./build/zonal-gateway-demo
 *
 *              NOTE: This demo is host tooling, not target code — it uses <cstdio>
 *              for console output. The production library it exercises remains
 *              fully AUTOSAR-constrained (no heap, no exceptions, integer-only).
 * @project     Deterministic TSN Zonal Backbone
 * @standards   ISO 26262 ASIL-D (demonstration of safety lifecycle)
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * =====================================================================================
 */

#include <cstdio>
#include "norxs/tsn/TsnOrchestrator.hpp"

using namespace norxs::tsn;

// ─────────────────────────────────────────────────────────────────────────────
// Simulated switch ASIC — stands in for SJA1110 / 88Q5050 on the host
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr RegAddr kPhcSecHighReg = 0x0408U;
constexpr RegAddr kPhcSecLowReg  = 0x040CU;
constexpr RegAddr kPhcNsReg      = 0x0410U;

class SimulatedSwitchAsic final : public SwitchHal
{
public:
    u32 phcSeconds { 0U };
    ScheduleParams hwGcl[kMaxSwitchPorts] {};

    Status Init() noexcept override
    {
        std::printf("  [HAL] chip reset + register self-test ... OK\n");
        return Status::Ok();
    }

    Status WriteRegister(u8, RegAddr, RegData) noexcept override { return Status::Ok(); }

    Status ReadRegister(u8, RegAddr address, RegData& outData) const noexcept override
    {
        switch (address)
        {
            case kPhcSecHighReg: outData = 0U;         break;
            case kPhcSecLowReg:  outData = phcSeconds; break;
            case kPhcNsReg:      outData = 0U;         break;
            default:             outData = 0U;         break;
        }
        return Status::Ok();
    }

    Status ReadModifyWrite(u8, RegAddr, RegData, RegData) noexcept override { return Status::Ok(); }

    Status ApplyGclToPort(u8 port, const ScheduleParams& params) noexcept override
    {
        std::printf("  [HAL] port %u: GCL committed — %u entries, cycle %llu ns\n",
                    static_cast<unsigned>(port),
                    static_cast<unsigned>(params.entryCount),
                    static_cast<unsigned long long>(params.cycleTimeNs));
        if (port < static_cast<u8>(kMaxSwitchPorts)) { hwGcl[port] = params; }
        return Status::Ok();
    }

    Status ReadOperGcl(u8 port, ScheduleParams& out) const noexcept override
    {
        out = (port < static_cast<u8>(kMaxSwitchPorts)) ? hwGcl[port] : ScheduleParams{};
        return Status::Ok();
    }

    Status IsGclSwapPending(u8, bool& out) const noexcept override { out = false; return Status::Ok(); }
    Status SetStreamFilter(const StreamFilterInstance&) noexcept override { return Status::Ok(); }
    Status ClearStreamFilter(u16) noexcept override { return Status::Ok(); }
    Status GetStreamDropCount(u16, u64& out) const noexcept override { out = 0ULL; return Status::Ok(); }
    Status RegisterFrerStream(const FrerStreamEntry& e) noexcept override
    {
        std::printf("  [HAL] FRER stream %u registered (%u disjoint paths)\n",
                    static_cast<unsigned>(e.streamHandle),
                    static_cast<unsigned>(e.memberPaths));
        return Status::Ok();
    }
    Status DeregisterFrerStream(u16) noexcept override { return Status::Ok(); }
    Status GetFrerStats(u16, u64& a, u64& b, u64& c) const noexcept override
        { a = b = c = 0ULL; return Status::Ok(); }
    Status ConfigureMacsecChannel(const MacsecChannelConfig&) noexcept override { return Status::Ok(); }
    Status RotateMacsecKey(u8, const MacsecSak128&) noexcept override { return Status::Ok(); }
    Status GetMacsecAuthFailures(u8, u64& out) const noexcept override { out = 0ULL; return Status::Ok(); }
    Status SetVlanEntry(u16, u16, u16) noexcept override { return Status::Ok(); }
    Status SetTcamRule(u8, const std::array<u8, 6U>&, u16, bool) noexcept override { return Status::Ok(); }
    Status GetPortDropCount(u8, u64& out) const noexcept override { out = 0ULL; return Status::Ok(); }
    Status GetLinkStatus(u8, bool& up, u32& spd) const noexcept override
        { up = true; spd = 1000U; return Status::Ok(); }
    const char* GetDeviceIdentifier() const noexcept override { return "SimulatedSwitchAsic"; }
    u8 GetPortCount() const noexcept override { return 8U; }
};

const char* StateName(OrchestratorState s)
{
    switch (s)
    {
        case OrchestratorState::kUninitialized: return "kUninitialized";
        case OrchestratorState::kInitializing:  return "kInitializing";
        case OrchestratorState::kPtpConverging: return "kPtpConverging";
        case OrchestratorState::kScheduleArmed: return "kScheduleArmed";
        case OrchestratorState::kRunning:       return "kRunning";
        case OrchestratorState::kDegraded:      return "kDegraded";
        case OrchestratorState::kSafeState:     return "kSafeState";
        case OrchestratorState::kFault:         return "kFault";
    }
    return "?";
}

ScheduleParams MakeProductionSchedule()
{
    // 1 ms cycle: guard band → TC7 safety window → AVB → best effort
    ScheduleParams p{};
    p.cycleTimeNs             = 1'000'000ULL;
    p.baseTime                = kPtpTimestampZero;
    p.maxSafetyFrameSizeBytes = 100U;
    p.entryCount              = 4U;
    p.gcl[0] = GclEntry{ kAllQueuesClosed,  20'000U };  // guard band
    p.gcl[1] = GclEntry{ kSafetyOnlyOpen,   30'000U };  // TC7: Brake-by-Wire
    p.gcl[2] = GclEntry{ 0x30U,            100'000U };  // TC4/TC5: AVB video
    p.gcl[3] = GclEntry{ kAllQueuesOpen,   850'000U };  // best effort
    return p;
}

OrchestratorConfig MakeGatewayConfig()
{
    OrchestratorConfig cfg{};
    cfg.ptp.localClockQuality.clockClass    = 6U;   // GM-grade (GNSS-backed)
    cfg.ptp.localClockQuality.clockAccuracy = 0x20U;
    cfg.ptp.priority1           = 128U;
    cfg.ptp.priority2           = 128U;
    cfg.ptp.portCount           = 2U;
    cfg.ptp.syncTimeoutNs       = 10'000'000ULL;
    cfg.ptp.holdoverBudgetNs    = 100'000'000ULL;
    cfg.ptp.driftCompensationNs = 1'000ULL;
    cfg.ptp.ports[0] = PtpPortDataset{};
    cfg.ptp.ports[0].portIndex = 0U;
    cfg.ptp.ports[0].enabled   = true;
    cfg.ptp.ports[1].portIndex = 1U;
    cfg.ptp.ports[1].enabled   = true;

    cfg.schedulePortCount = 2U;
    cfg.schedules[0] = MakeProductionSchedule();
    cfg.schedules[1] = MakeProductionSchedule();

    cfg.frerStreamCount = 1U;
    cfg.frerStreams[0].streamHandle = 42U;
    cfg.frerStreams[0].memberPaths  = 2U;

    cfg.streamFilterCount = 1U;
    cfg.streamFilters[0].streamHandle = 42U;

    cfg.safeStateSchedule       = MakeProductionSchedule();
    cfg.tickPeriodUs            = 125U;
    cfg.ptpConvergenceTimeoutMs = 500U;
    return cfg;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

int main()
{
    std::printf("══════════════════════════════════════════════════════════════\n");
    std::printf(" norxs Deterministic TSN Zonal Backbone — lifecycle demo\n");
    std::printf("══════════════════════════════════════════════════════════════\n\n");

    SimulatedSwitchAsic asic;
    TsnOrchestrator orch{ asic };

    std::printf("[1] Init() — 8-phase bring-up, NC pre-validation\n");
    if (orch.Init(MakeGatewayConfig()).IsError())
    {
        std::printf("    Init FAILED\n");
        return 1;
    }
    std::printf("    state = %s\n\n", StateName(orch.GetState()));

    std::printf("[2] Start() — begin gPTP convergence\n");
    (void)orch.Start();
    std::printf("    state = %s\n\n", StateName(orch.GetState()));

    std::printf("[3] 125 us scheduling loop — BMCA self-election, GCL load\n");
    for (int i = 0; i < 3 && !orch.IsFullyOperational(); ++i) { (void)orch.Tick(); }
    std::printf("    state = %s  (fully operational: %s)\n\n",
                StateName(orch.GetState()),
                orch.IsFullyOperational() ? "yes" : "no");

    std::printf("[4] Fault injection — flip one interval in the hardware GCL\n");
    asic.hwGcl[0].gcl[2].intervalNs += 1U;
    asic.phcSeconds = 10U;                       // next integrity-check window
    (void)orch.Tick();
    std::printf("    state = %s  (integrity fault detected)\n\n",
                StateName(orch.GetState()));

    std::printf("[5] Fault persists — NC guarantees void, enter safe state\n");
    asic.phcSeconds = 11U;
    (void)orch.Tick();
    std::printf("    state = %s  (TC7-exclusive schedule re-applied)\n\n",
                StateName(orch.GetState()));

    std::printf("[6] ISO 26262 \u00a77.4.2 audit trail\n");
    std::array<AuditEntry, kAuditLogSize> log{};
    u8 count = 0U;
    (void)orch.GetAuditLog(log, count);
    for (u8 i = 0U; i < count; ++i)
    {
        std::printf("    #%u  %-15s -> %-12s  trigger=%u\n",
                    static_cast<unsigned>(i),
                    StateName(log[i].previousState),
                    StateName(log[i].newState),
                    static_cast<unsigned>(log[i].triggerCode));
    }

    std::printf("\nDemo complete.\n");
    return 0;
}

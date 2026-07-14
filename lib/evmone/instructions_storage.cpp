// evmone: Fast Ethereum Virtual Machine implementation
// Copyright 2019 The evmone Authors.
// SPDX-License-Identifier: Apache-2.0

#include "instructions.hpp"

namespace evmone::instr::core
{
namespace
{
/// The gas cost specification for storage instructions.
struct StorageCostSpec
{
    bool net_cost;        ///< Is this net gas cost metering schedule?
    int16_t warm_access;  ///< Storage warm access cost, YP: G_{warmaccess}
    int16_t set;          ///< Storage addition cost, YP: G_{sset}
    int16_t reset;        ///< Storage modification cost, YP: G_{sreset}
    int16_t clear;        ///< Storage deletion refund, YP: R_{sclear}
};

/// Table of gas cost specification for storage instructions per EVM revision.
/// TODO: This can be moved to instruction traits and be used in other places: e.g.
///       SLOAD cost, replacement for warm_storage_read_cost.
constexpr auto storage_cost_spec = []() noexcept {
    std::array<StorageCostSpec, EVMC_MAX_REVISION + 1> tbl{};

    // Legacy cost schedule.
    for (auto rev : {EVMC_FRONTIER, EVMC_HOMESTEAD, EVMC_TANGERINE_WHISTLE, EVMC_SPURIOUS_DRAGON,
             EVMC_BYZANTIUM, EVMC_PETERSBURG})
        tbl[rev] = {false, 200, 20000, 5000, 15000};

    // Net cost schedule.
    tbl[EVMC_CONSTANTINOPLE] = {true, 200, 20000, 5000, 15000};
    tbl[EVMC_ISTANBUL] = {true, 800, 20000, 5000, 15000};
    tbl[EVMC_BERLIN] = {
        true, instr::warm_storage_read_cost, 20000, 5000 - instr::cold_sload_cost, 15000};
    tbl[EVMC_LONDON] = {
        true, instr::warm_storage_read_cost, 20000, 5000 - instr::cold_sload_cost, 4800};
    tbl[EVMC_PARIS] = tbl[EVMC_LONDON];
    tbl[EVMC_SHANGHAI] = tbl[EVMC_LONDON];
    tbl[EVMC_CANCUN] = tbl[EVMC_LONDON];
    tbl[EVMC_PRAGUE] = tbl[EVMC_LONDON];
    tbl[EVMC_OSAKA] = tbl[EVMC_LONDON];
    tbl[EVMC_AMSTERDAM] = tbl[EVMC_LONDON];
    tbl[EVMC_EXPERIMENTAL] = tbl[EVMC_LONDON];
    return tbl;
}();


struct StorageStoreCost
{
    int16_t gas_cost;
    int16_t gas_refund;
};

// The lookup table of SSTORE costs by the storage update status.
constexpr auto sstore_costs = []() noexcept {
    std::array<std::array<StorageStoreCost, EVMC_STORAGE_MODIFIED_RESTORED + 1>,
        EVMC_MAX_REVISION + 1>
        tbl{};

    for (size_t rev = EVMC_FRONTIER; rev <= EVMC_MAX_REVISION; ++rev)
    {
        auto& e = tbl[rev];
        if (const auto c = storage_cost_spec[rev]; !c.net_cost)  // legacy
        {
            e[EVMC_STORAGE_ADDED] = {c.set, 0};
            e[EVMC_STORAGE_DELETED] = {c.reset, c.clear};
            e[EVMC_STORAGE_MODIFIED] = {c.reset, 0};
            e[EVMC_STORAGE_ASSIGNED] = e[EVMC_STORAGE_MODIFIED];
            e[EVMC_STORAGE_DELETED_ADDED] = e[EVMC_STORAGE_ADDED];
            e[EVMC_STORAGE_MODIFIED_DELETED] = e[EVMC_STORAGE_DELETED];
            e[EVMC_STORAGE_DELETED_RESTORED] = e[EVMC_STORAGE_ADDED];
            e[EVMC_STORAGE_ADDED_DELETED] = e[EVMC_STORAGE_DELETED];
            e[EVMC_STORAGE_MODIFIED_RESTORED] = e[EVMC_STORAGE_MODIFIED];
        }
        else  // net cost
        {
            e[EVMC_STORAGE_ASSIGNED] = {c.warm_access, 0};
            e[EVMC_STORAGE_ADDED] = {c.set, 0};
            e[EVMC_STORAGE_DELETED] = {c.reset, c.clear};
            e[EVMC_STORAGE_MODIFIED] = {c.reset, 0};
            e[EVMC_STORAGE_DELETED_ADDED] = {c.warm_access, static_cast<int16_t>(-c.clear)};
            e[EVMC_STORAGE_MODIFIED_DELETED] = {c.warm_access, c.clear};
            e[EVMC_STORAGE_DELETED_RESTORED] = {
                c.warm_access, static_cast<int16_t>(c.reset - c.warm_access - c.clear)};
            e[EVMC_STORAGE_ADDED_DELETED] = {
                c.warm_access, static_cast<int16_t>(c.set - c.warm_access)};
            e[EVMC_STORAGE_MODIFIED_RESTORED] = {
                c.warm_access, static_cast<int16_t>(c.reset - c.warm_access)};
        }
    }

    return tbl;
}();
}  // namespace

Result sload(StackTop stack, int64_t gas_left, ExecutionState& state) noexcept
{
    auto& x = stack.top();
    const auto key = intx::be::store<evmc::bytes32>(x);

    if (state.rev >= EVMC_BERLIN)
    {
        // Peek without recording a BAL read: EELS charges the access cost
        // before performing the storage read, so an OOG here leaves no trace.
        g_suppress_bal_observer = true;
        const auto slot_status = state.host.access_storage(state.msg->recipient, key);
        g_suppress_bal_observer = false;
        if (slot_status == EVMC_ACCESS_COLD)
        {
            // The warm storage access cost is already applied (from the cost table).
            // Here we need to apply additional cold storage access cost.
            // EIP-8038 (Amsterdam): COLD_STORAGE_ACCESS raised from 2100 to 3000.
            constexpr auto additional_cold_sload_cost =
                instr::cold_sload_cost - instr::warm_storage_read_cost;
            constexpr auto additional_cold_sload_cost_amsterdam =
                int64_t{3000} - instr::warm_storage_read_cost;
            const auto extra_cost = (state.rev >= EVMC_AMSTERDAM)
                                        ? additional_cold_sload_cost_amsterdam
                                        : int64_t{additional_cold_sload_cost};
            if ((gas_left -= extra_cost) < 0)
                return {EVMC_OUT_OF_GAS, gas_left};
        }
        state.host.access_storage(state.msg->recipient, key);
    }

    x = intx::be::load<uint256>(state.host.get_storage(state.msg->recipient, key));

    return {EVMC_SUCCESS, gas_left};
}

Result sstore(StackTop stack, int64_t gas_left, ExecutionState& state) noexcept
{
    if (state.in_static_mode())
        return {EVMC_STATIC_MODE_VIOLATION, gas_left};

    if (state.rev >= EVMC_ISTANBUL && state.rev < EVMC_AMSTERDAM && gas_left <= 2300)
        return {EVMC_OUT_OF_GAS, gas_left};

    const auto key = intx::be::store<evmc::bytes32>(stack.pop());
    const auto value = intx::be::store<evmc::bytes32>(stack.pop());

    // EIP-8038 (Amsterdam): COLD_STORAGE_ACCESS raised from 2100 to 3000.
    // Charged as a surcharge over the warm cost baked into every row of the
    // Amsterdam SSTORE table below (cold total = warm + 2900).
    const auto cold_access_cost =
        (state.rev >= EVMC_AMSTERDAM) ? int64_t{2900} : int64_t{instr::cold_sload_cost};
    int64_t gas_cost_cold = 0;
    if (state.rev >= EVMC_AMSTERDAM)
    {
        // EELS devnet-7 sstore: the access cost must be affordable before the
        // storage read records the slot in the BAL. Post-repricing the cold
        // cost exceeds the EIP-2200 stipend, so the stipend sentry
        // (gas_left > CALL_STIPEND) folds into the same check.
        g_suppress_bal_observer = true;
        const auto slot_status = state.host.access_storage(state.msg->recipient, key);
        g_suppress_bal_observer = false;
        const int64_t access_cost = (slot_status == EVMC_ACCESS_COLD) ? 3000 : 100;
        const int64_t check = std::max<int64_t>(access_cost, 2301);
        if (gas_left < check)
            return {EVMC_OUT_OF_GAS, gas_left - check};
        if (slot_status == EVMC_ACCESS_COLD)
            gas_cost_cold = cold_access_cost;
        // Record the slot read now that the access check passed.
        state.host.access_storage(state.msg->recipient, key);
    }
    else if (state.rev >= EVMC_BERLIN &&
             state.host.access_storage(state.msg->recipient, key) == EVMC_ACCESS_COLD)
    {
        gas_cost_cold = cold_access_cost;
    }
    const auto status = state.host.set_storage(state.msg->recipient, key, value);

    auto [gas_cost_warm, gas_refund] = sstore_costs[state.rev][status];

    // EIP-8037 + EIP-8038 (Amsterdam): SSTORE pricing overhaul.
    //   STORAGE_WRITE = 10,000 (up from 2,800), added on the first change in a tx
    //   STORAGE_CLEAR_REFUND = 12,480 (up from 4,800)
    //   STATE_GAS on 0->non-zero = 97,920 (EIP-8037 STATE_BYTES_PER_STORAGE_SET × CPSB)
    // State gas is drawn from the same gas_left here (no separate reservoir in
    // evmone) — silkworm's post-hoc counter attributes it to the state dimension
    // for the block-level max(regular, state) check.
    int64_t state_gas_cost = 0;
    if (state.rev >= EVMC_AMSTERDAM)
    {
        constexpr int64_t kStorageWrite = 10000;      // EIP-8038 STORAGE_WRITE
        constexpr int64_t kClearRefund = 12480;       // EIP-8038 STORAGE_CLEAR_REFUND
        constexpr int64_t kWarmAccess = 100;          // EIP-8038 WARM_ACCESS
        constexpr int64_t kStateSet = 97920;          // EIP-8037 GAS_STORAGE_SET (state)
        // First-change rows charge WARM_ACCESS + STORAGE_WRITE (10,100);
        // the cold surcharge (2,900) on top brings the cold total to 13,000.
        switch (status)
        {
        case EVMC_STORAGE_ADDED:                       // 0 -> 0 -> x: new slot
            gas_cost_warm = kWarmAccess + kStorageWrite;
            gas_refund = 0;
            state_gas_cost = kStateSet;
            break;
        case EVMC_STORAGE_DELETED:                     // x -> x -> 0: clear (first change)
            gas_cost_warm = kWarmAccess + kStorageWrite;
            gas_refund = kClearRefund;
            break;
        case EVMC_STORAGE_MODIFIED:                    // x -> x -> y: first change of existing slot
            gas_cost_warm = kWarmAccess + kStorageWrite;
            gas_refund = 0;
            break;
        case EVMC_STORAGE_ASSIGNED:                    // dirty update (no first-change)
            gas_cost_warm = kWarmAccess;
            gas_refund = 0;
            break;
        case EVMC_STORAGE_DELETED_ADDED:               // x -> 0 -> y: cleared slot re-added
            gas_cost_warm = kWarmAccess;
            gas_refund = -kClearRefund;                // reverse the earlier clear-refund
            break;
        case EVMC_STORAGE_MODIFIED_DELETED:            // x -> y -> 0: dirty then clear
            gas_cost_warm = kWarmAccess;
            gas_refund = kClearRefund;
            break;
        case EVMC_STORAGE_DELETED_RESTORED:            // x -> 0 -> x: cleared then restored
            gas_cost_warm = kWarmAccess;
            gas_refund = kStorageWrite - kClearRefund; // refund STORAGE_WRITE, reverse clear
            break;
        case EVMC_STORAGE_ADDED_DELETED:               // 0 -> x -> 0: added then cleared within tx
            gas_cost_warm = kWarmAccess;
            gas_refund = kStorageWrite;                // refund STORAGE_WRITE
            state_gas_cost = -kStateSet;               // refill state gas (LIFO)
            break;
        case EVMC_STORAGE_MODIFIED_RESTORED:           // x -> y -> x: dirty then restored
            gas_cost_warm = kWarmAccess;
            gas_refund = kStorageWrite;
            break;
        }
    }
    const auto regular_cost = static_cast<int64_t>(gas_cost_warm) + gas_cost_cold;

    // EIP-8037 reservoir model: state-gas charges/refills draw from the tx-wide
    // reservoir first; only the shortfall bites gas_left.
    if (state_gas_cost > 0 && state.state_gas_reservoir != nullptr)
    {
        // Affordability first: an OOG here must not leak a phantom charge
        // into the tx-wide counters or the reservoir.
        const int64_t from_reservoir = std::min(state_gas_cost, *state.state_gas_reservoir);
        const int64_t remaining = state_gas_cost - from_reservoir;
        if (gas_left < regular_cost + remaining)
            return {EVMC_OUT_OF_GAS, gas_left - regular_cost - remaining};
        gas_left -= regular_cost + remaining;
        *state.state_gas_reservoir -= from_reservoir;
        g_tx_state_gas_charged += state_gas_cost;  // gross, kept across refills
        state.state_gas_from_gas_left += remaining;
        state.state_gas_from_reservoir += from_reservoir;
        state.state_gas_used_net += state_gas_cost;
    }
    else if (state_gas_cost < 0 && state.state_gas_reservoir != nullptr)
    {
        // Refill LIFO: credit gas_left up to state_gas_from_gas_left, remainder to reservoir.
        int64_t refill = -state_gas_cost;
        if ((gas_left -= regular_cost) < 0)
            return {EVMC_OUT_OF_GAS, gas_left};
        const int64_t to_gas_left = std::min(refill, state.state_gas_from_gas_left);
        gas_left += to_gas_left;
        state.state_gas_from_gas_left -= to_gas_left;
        const int64_t to_reservoir = refill - to_gas_left;
        *state.state_gas_reservoir += to_reservoir;
        state.state_gas_from_reservoir -= std::min(to_reservoir, state.state_gas_from_reservoir);
        g_tx_state_gas_refilled_gas_left += to_gas_left;
        g_tx_state_gas_refilled_reservoir += to_reservoir;
        state.state_gas_used_net -= refill;
    }
    else
    {
        // Pre-Amsterdam or no reservoir attached: legacy path.
        const auto gas_cost = regular_cost + state_gas_cost;
        if ((gas_left -= gas_cost) < 0)
            return {EVMC_OUT_OF_GAS, gas_left};
    }
    state.gas_refund += gas_refund;
    return {EVMC_SUCCESS, gas_left};
}
}  // namespace evmone::instr::core

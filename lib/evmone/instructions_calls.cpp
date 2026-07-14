// evmone: Fast Ethereum Virtual Machine implementation
// Copyright 2019 The evmone Authors.
// SPDX-License-Identifier: Apache-2.0

#include "delegation.hpp"
#include "instructions.hpp"
#include <variant>

constexpr int64_t CALL_VALUE_COST = 9000;
constexpr int64_t ACCOUNT_CREATION_COST = 25000;

namespace evmone::instr::core
{
namespace
{
/// Get target address of a code executing instruction.
///
/// Returns EIP-7702 delegate address if addr is delegated, or addr itself otherwise.
/// Applies gas charge for accessing delegate account and may fail with out of gas.
inline std::variant<evmc::address, Result> get_target_address(
    const evmc::address& addr, int64_t& gas_left, ExecutionState& state) noexcept
{
    if (state.rev < EVMC_PRAGUE)
        return addr;

    const auto delegate_addr = get_delegate_address(state.host, addr);
    if (!delegate_addr)
        return addr;

    // Peek the access status without recording a BAL read: the read only
    // counts once the gas check passes (EELS reads the delegate account
    // after check_gas). The warm-marking is journal-rolled-back on OOG.
    g_suppress_bal_observer = true;
    const auto access_status = state.host.access_account(*delegate_addr);
    g_suppress_bal_observer = false;
    const auto delegate_account_access_cost =
        (access_status == EVMC_ACCESS_COLD ?
                instr::cold_account_access_cost_for(state.rev) :
                int64_t{instr::warm_storage_read_cost});

    if ((gas_left -= delegate_account_access_cost) < 0)
        return Result{EVMC_OUT_OF_GAS, gas_left};

    return *delegate_addr;
}
}  // namespace

/// Converts an opcode to matching EVMC call kind.
/// NOLINTNEXTLINE(misc-use-internal-linkage) fixed in clang-tidy 20.
consteval evmc_call_kind to_call_kind(Opcode op) noexcept
{
    switch (op)
    {
    case OP_CALL:
    case OP_STATICCALL:
        return EVMC_CALL;
    case OP_CALLCODE:
        return EVMC_CALLCODE;
    case OP_DELEGATECALL:
        return EVMC_DELEGATECALL;
    case OP_CREATE:
        return EVMC_CREATE;
    case OP_CREATE2:
        return EVMC_CREATE2;
    default:
        intx::unreachable();
    }
}

template <Opcode Op>
Result call_impl(StackTop stack, int64_t gas_left, ExecutionState& state) noexcept
{
    static_assert(
        Op == OP_CALL || Op == OP_CALLCODE || Op == OP_DELEGATECALL || Op == OP_STATICCALL);

    static constexpr bool HAS_VALUE_ARG = Op == OP_CALL || Op == OP_CALLCODE;

    const auto gas = stack.pop();
    const auto dst = intx::be::trunc<evmc::address>(stack.pop());
    const auto value = (!HAS_VALUE_ARG) ? 0 : stack.pop();
    const auto has_value = value != 0;
    const auto input_offset_u256 = stack.pop();
    const auto input_size_u256 = stack.pop();
    const auto output_offset_u256 = stack.pop();
    const auto output_size_u256 = stack.pop();

    stack.push(0);  // Assume failure.
    state.return_data.clear();

    // EIP-8037: LIFO refill of the NEW_ACCOUNT state charge when the call
    // does not create the account after all (light failure or child failure).
    bool new_account_charged = false;
    const auto refill_new_account = [&state, &gas_left, &new_account_charged]() noexcept {
        if (!new_account_charged)
            return;
        constexpr int64_t amount = 183600;
        const int64_t to_gas_left = std::min(amount, state.state_gas_from_gas_left);
        gas_left += to_gas_left;
        state.state_gas_from_gas_left -= to_gas_left;
        const int64_t to_reservoir = amount - to_gas_left;
        if (state.state_gas_reservoir != nullptr)
        {
            *state.state_gas_reservoir += to_reservoir;
            state.state_gas_from_reservoir -=
                std::min(to_reservoir, state.state_gas_from_reservoir);
        }
        g_tx_state_gas_refilled_gas_left += to_gas_left;
        g_tx_state_gas_refilled_reservoir += to_reservoir;
        state.state_gas_used_net -= amount;
        new_account_charged = false;
    };

    if (state.rev >= EVMC_BERLIN)
    {
        g_suppress_bal_observer = true;
        const auto dst_status = state.host.access_account(dst);
        g_suppress_bal_observer = false;
        if (dst_status == EVMC_ACCESS_COLD &&
            (gas_left -= instr::additional_cold_account_access_cost_for(state.rev)) < 0)
            return {EVMC_OUT_OF_GAS, gas_left};
    }

    if (!check_memory(gas_left, state.memory, input_offset_u256, input_size_u256))
        return {EVMC_OUT_OF_GAS, gas_left};

    if (!check_memory(gas_left, state.memory, output_offset_u256, output_size_u256))
        return {EVMC_OUT_OF_GAS, gas_left};

    if constexpr (HAS_VALUE_ARG)
    {
        // EIP-8038 (Amsterdam): the value-transfer surcharge becomes
        // ACCOUNT_WRITE (8,000) + stipend (2,300) = 10,300 (was 9,000).
        const auto call_value_cost =
            (state.rev >= EVMC_AMSTERDAM) ? int64_t{10300} : int64_t{CALL_VALUE_COST};
        auto cost = has_value ? call_value_cost : int64_t{0};

        if constexpr (Op == OP_CALL)
        {
            if (has_value && state.in_static_mode())
                return {EVMC_STATIC_MODE_VIOLATION, gas_left};

            if (state.rev < EVMC_AMSTERDAM &&
                (has_value || state.rev < EVMC_SPURIOUS_DRAGON) && !state.host.account_exists(dst))
                cost += ACCOUNT_CREATION_COST;
        }

        if ((gas_left -= cost) < 0)
            return {EVMC_OUT_OF_GAS, gas_left};
    }

    // EELS `call` ordering: the target state read happens after the combined
    // access + transfer + memory gas check passes; the delegate resolution
    // (and its own gas check) follows, and the delegate read comes after it.
    state.host.access_account(dst);

    const auto target_addr_or_result = get_target_address(dst, gas_left, state);
    if (const auto* result = std::get_if<Result>(&target_addr_or_result))
        return *result;

    const auto& code_addr = std::get<evmc::address>(target_addr_or_result);
    if (code_addr != dst)
        state.host.access_account(code_addr);

    if constexpr (Op == OP_CALL)
    {
        // EIP-8037 (Amsterdam, per EELS `call`): NEW_ACCOUNT state gas for
        // value to a non-alive account is charged in the PARENT before the
        // 63/64 split so the child budget shrinks accordingly. Refilled
        // below on light failure or child failure.
        if (state.rev >= EVMC_AMSTERDAM && has_value && !state.host.account_exists(dst))
        {
            constexpr int64_t new_account_state_cost = 183600;
            const int64_t from_reservoir =
                state.state_gas_reservoir != nullptr
                    ? std::min(new_account_state_cost, *state.state_gas_reservoir)
                    : 0;
            const int64_t remaining = new_account_state_cost - from_reservoir;
            if (gas_left < remaining)
                return {EVMC_OUT_OF_GAS, gas_left - remaining};
            gas_left -= remaining;
            if (state.state_gas_reservoir != nullptr)
                *state.state_gas_reservoir -= from_reservoir;
            g_tx_state_gas_charged += new_account_state_cost;
            state.state_gas_from_reservoir += from_reservoir;
            state.state_gas_from_gas_left += remaining;
            state.state_gas_used_net += new_account_state_cost;
            new_account_charged = true;
        }
    }

    const auto input_offset = static_cast<size_t>(input_offset_u256);
    const auto input_size = static_cast<size_t>(input_size_u256);
    const auto output_offset = static_cast<size_t>(output_offset_u256);
    const auto output_size = static_cast<size_t>(output_size_u256);

    evmc_message msg{.kind = to_call_kind(Op)};
    msg.flags = (Op == OP_STATICCALL) ? uint32_t{EVMC_STATIC} : state.msg->flags;
    if (dst != code_addr)
        msg.flags |= EVMC_DELEGATED;
    else
        msg.flags &= ~std::underlying_type_t<evmc_flags>{EVMC_DELEGATED};
    msg.depth = state.msg->depth + 1;
    msg.recipient = (Op == OP_CALL || Op == OP_STATICCALL) ? dst : state.msg->recipient;
    msg.code_address = code_addr;
    msg.sender = (Op == OP_DELEGATECALL) ? state.msg->sender : state.msg->recipient;
    msg.value =
        (Op == OP_DELEGATECALL) ? state.msg->value : intx::be::store<evmc::uint256be>(value);

    if (input_size > 0)
    {
        // input_offset may be garbage if input_size == 0.
        msg.input_data = &state.memory[input_offset];
        msg.input_size = input_size;
    }

    msg.gas = std::numeric_limits<int64_t>::max();
    if (gas < msg.gas)
        msg.gas = static_cast<int64_t>(gas);

    if constexpr (Op == OP_STATICCALL)
    {
        msg.gas = std::min(msg.gas, gas_left - gas_left / 64);
    }
    else
    {
        if (state.rev >= EVMC_TANGERINE_WHISTLE)  // Always true for STATICCALL.
            msg.gas = std::min(msg.gas, gas_left - gas_left / 64);
        else if (msg.gas > gas_left)
            return {EVMC_OUT_OF_GAS, gas_left};
    }

    if constexpr (HAS_VALUE_ARG)
    {
        if (has_value)
        {
            msg.gas += 2300;  // Add stipend.
            gas_left += 2300;
            if (intx::be::load<uint256>(state.host.get_balance(state.msg->recipient)) < value)
            {
                refill_new_account();
                return {EVMC_SUCCESS, gas_left};  // "Light" failure.
            }
        }
    }

    if (state.msg->depth >= 1024)
    {
        refill_new_account();
        return {EVMC_SUCCESS, gas_left};  // "Light" failure.
    }

    // Clear before dispatching so precompile / empty-code paths that bypass
    // baseline::execute leave a zero for our merge below.
    g_last_frame_state_gas_from_gas_left = 0;
    g_last_frame_state_gas_from_reservoir = 0;
    g_last_frame_state_gas_used_net = 0;
    const auto result = state.host.call(msg);
    state.return_data.assign(result.output_data, result.output_size);
    stack.top() = result.status_code == EVMC_SUCCESS;

    if (const auto copy_size = std::min(output_size, result.output_size); copy_size > 0)
        std::memcpy(&state.memory[output_offset], result.output_data, copy_size);

    const auto gas_used = msg.gas - result.gas_left;
    gas_left -= gas_used;
    state.gas_refund += result.gas_refund;

    // EIP-8037: fold the child's surviving state-gas draws into our own frame
    // counters so a later failure of THIS frame refunds them too. On child
    // non-success the child already refilled and published zero.
    state.state_gas_from_gas_left += g_last_frame_state_gas_from_gas_left;
    state.state_gas_from_reservoir += g_last_frame_state_gas_from_reservoir;
    state.state_gas_used_net += g_last_frame_state_gas_used_net;
    g_last_frame_state_gas_from_gas_left = 0;
    g_last_frame_state_gas_from_reservoir = 0;
    g_last_frame_state_gas_used_net = 0;

    // Child failed — the account was not created; refill its NEW_ACCOUNT charge.
    if (result.status_code != EVMC_SUCCESS)
        refill_new_account();

    return {EVMC_SUCCESS, gas_left};
}

template Result call_impl<OP_CALL>(
    StackTop stack, int64_t gas_left, ExecutionState& state) noexcept;
template Result call_impl<OP_STATICCALL>(
    StackTop stack, int64_t gas_left, ExecutionState& state) noexcept;
template Result call_impl<OP_DELEGATECALL>(
    StackTop stack, int64_t gas_left, ExecutionState& state) noexcept;
template Result call_impl<OP_CALLCODE>(
    StackTop stack, int64_t gas_left, ExecutionState& state) noexcept;

template <Opcode Op>
Result create_impl(StackTop stack, int64_t gas_left, ExecutionState& state) noexcept
{
    static_assert(Op == OP_CREATE || Op == OP_CREATE2);

    if (state.in_static_mode())
        return {EVMC_STATIC_MODE_VIOLATION, gas_left};

    const auto endowment = stack.pop();
    const auto init_code_offset_u256 = stack.pop();
    const auto init_code_size_u256 = stack.pop();
    const auto salt = (Op == OP_CREATE2) ? stack.pop() : uint256{};

    stack.push(0);  // Assume failure.
    state.return_data.clear();

    if (!check_memory(gas_left, state.memory, init_code_offset_u256, init_code_size_u256))
        return {EVMC_OUT_OF_GAS, gas_left};

    const auto init_code_offset = static_cast<size_t>(init_code_offset_u256);
    const auto init_code_size = static_cast<size_t>(init_code_size_u256);

    // EIP-3860 initcode limit; EIP-7954 (Amsterdam) raises it to 128 KiB.
    const size_t max_initcode_size = (state.rev >= EVMC_AMSTERDAM) ? 0x20000 : 0xC000;
    if (state.rev >= EVMC_SHANGHAI && init_code_size > max_initcode_size)
        return {EVMC_OUT_OF_GAS, gas_left};

    const auto init_code_word_cost = 6 * (Op == OP_CREATE2) + 2 * (state.rev >= EVMC_SHANGHAI);
    const auto init_code_cost = num_words(init_code_size) * init_code_word_cost;
    if ((gas_left -= init_code_cost) < 0)
        return {EVMC_OUT_OF_GAS, gas_left};

    // EELS generic_create: light failures (depth, balance) are checked
    // before any state charge is attempted.
    if (state.msg->depth >= 1024)
        return {EVMC_SUCCESS, gas_left};  // "Light" failure.

    if (endowment != 0 &&
        intx::be::load<uint256>(state.host.get_balance(state.msg->recipient)) < endowment)
        return {EVMC_SUCCESS, gas_left};  // "Light" failure.

    // EIP-8037 (devnet-7, EELS generic_create): the account-creation state
    // charge is conditional on the target not being alive — decided by
    // existence alone, independently of the collision outcome — paid in the
    // PARENT before the 63/64 split and refilled when the child fails.
    bool create_state_charged = false;
    const auto refill_create_state = [&state, &gas_left, &create_state_charged]() noexcept {
        if (!create_state_charged)
            return;
        constexpr int64_t amount = 183600;
        const int64_t to_gas_left = std::min(amount, state.state_gas_from_gas_left);
        gas_left += to_gas_left;
        state.state_gas_from_gas_left -= to_gas_left;
        const int64_t to_reservoir = amount - to_gas_left;
        if (state.state_gas_reservoir != nullptr)
        {
            *state.state_gas_reservoir += to_reservoir;
            state.state_gas_from_reservoir -=
                std::min(to_reservoir, state.state_gas_from_reservoir);
        }
        g_tx_state_gas_refilled_gas_left += to_gas_left;
        g_tx_state_gas_refilled_reservoir += to_reservoir;
        state.state_gas_used_net -= amount;
        create_state_charged = false;
    };
    if (state.rev >= EVMC_AMSTERDAM)
    {
        bool target_alive = false;
        if (g_create_target_address_fn != nullptr)
        {
            bool sender_nonce_maxed = false;
            const auto target = g_create_target_address_fn(g_create_target_address_ctx,
                state.msg->recipient, Op == OP_CREATE2, intx::be::store<evmc::bytes32>(salt),
                init_code_size > 0 ? &state.memory[init_code_offset] : nullptr, init_code_size,
                &sender_nonce_maxed);
            // EIP-2681 nonce limit: a "light" failure before the target is
            // accessed — the aborted create must not warm or read it.
            if (sender_nonce_maxed)
                return {EVMC_SUCCESS, gas_left};
            // EELS generic_create marks the target accessed and reads its
            // aliveness before the charge — the read lands in the BAL even
            // when the create later fails or reverts.
            state.host.access_account(target);
            target_alive = state.host.account_exists(target);
        }
        if (!target_alive)
        {
            constexpr int64_t new_account_state_cost = 183600;
            const int64_t from_reservoir =
                state.state_gas_reservoir != nullptr
                    ? std::min(new_account_state_cost, *state.state_gas_reservoir)
                    : 0;
            const int64_t remaining = new_account_state_cost - from_reservoir;
            if (gas_left < remaining)
                return {EVMC_OUT_OF_GAS, gas_left - remaining};
            gas_left -= remaining;
            if (state.state_gas_reservoir != nullptr)
                *state.state_gas_reservoir -= from_reservoir;
            g_tx_state_gas_charged += new_account_state_cost;
            state.state_gas_from_reservoir += from_reservoir;
            state.state_gas_from_gas_left += remaining;
            state.state_gas_used_net += new_account_state_cost;
            create_state_charged = true;
        }
    }

    evmc_message msg{.kind = to_call_kind(Op)};
    msg.gas = gas_left;
    if (state.rev >= EVMC_TANGERINE_WHISTLE)
        msg.gas = msg.gas - msg.gas / 64;

    if (init_code_size > 0)
    {
        // init_code_offset may be garbage if init_code_size == 0.
        msg.input_data = &state.memory[init_code_offset];
        msg.input_size = init_code_size;
    }
    msg.sender = state.msg->recipient;
    msg.depth = state.msg->depth + 1;
    msg.create2_salt = intx::be::store<evmc::bytes32>(salt);
    msg.value = intx::be::store<evmc::uint256be>(endowment);

    g_last_frame_state_gas_from_gas_left = 0;
    g_last_frame_state_gas_from_reservoir = 0;
    g_last_frame_state_gas_used_net = 0;
    const auto result = state.host.call(msg);
    gas_left -= msg.gas - result.gas_left;
    state.gas_refund += result.gas_refund;

    state.return_data.assign(result.output_data, result.output_size);
    if (result.status_code == EVMC_SUCCESS)
        stack.top() = intx::be::load<uint256>(result.create_address);

    // EIP-8037: merge child's surviving state-gas draws into our frame's
    // counters (only the SUCCESS path publishes non-zero).
    state.state_gas_from_gas_left += g_last_frame_state_gas_from_gas_left;
    state.state_gas_from_reservoir += g_last_frame_state_gas_from_reservoir;
    state.state_gas_used_net += g_last_frame_state_gas_used_net;
    g_last_frame_state_gas_from_gas_left = 0;
    g_last_frame_state_gas_from_reservoir = 0;
    g_last_frame_state_gas_used_net = 0;

    // Refill the account-creation charge when no new leaf resulted (the
    // create failed — including collisions, which the Host reports as
    // failures).
    if (result.status_code != EVMC_SUCCESS)
        refill_create_state();

    return {EVMC_SUCCESS, gas_left};
}

template Result create_impl<OP_CREATE>(
    StackTop stack, int64_t gas_left, ExecutionState& state) noexcept;
template Result create_impl<OP_CREATE2>(
    StackTop stack, int64_t gas_left, ExecutionState& state) noexcept;
}  // namespace evmone::instr::core

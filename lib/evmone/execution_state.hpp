// evmone: Fast Ethereum Virtual Machine implementation
// Copyright 2019 The evmone Authors.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <evmc/evmc.hpp>
#include <intx/intx.hpp>
#include <exception>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace evmone
{
namespace advanced
{
struct AdvancedCodeAnalysis;
}
namespace baseline
{
class CodeAnalysis;
}

using evmc::bytes;
using evmc::bytes_view;
using intx::uint256;


/// Provides memory for EVM stack.
class StackSpace
{
    struct Storage
    {
        /// The maximum number of EVM stack items.
        static constexpr auto limit = 1024;

        /// Stack space items are aligned to 256 bits for better packing in cache lines.
        static constexpr auto alignment = sizeof(uint256);

        alignas(alignment) uint256 items[limit];
    };

    /// The storage allocated for maximum possible number of items.
    std::unique_ptr<Storage> m_stack_space = std::make_unique<Storage>();

public:
    static constexpr auto limit = Storage::limit;

    /// Returns the pointer to the "bottom", i.e. below the stack space.
    [[nodiscard]] uint256* bottom() noexcept { return &m_stack_space->items[0]; }
};


/// The EVM memory.
///
/// The implementations uses initial allocation of 4k and then grows capacity with 2x factor.
/// Some benchmarks have been done to confirm 4k is ok-ish value.
class Memory
{
    /// The size of allocation "page".
    static constexpr size_t page_size = 4 * 1024;

    struct FreeDeleter
    {
        void operator()(uint8_t* p) const noexcept { std::free(p); }
    };

    /// Owned pointer to allocated memory.
    std::unique_ptr<uint8_t[], FreeDeleter> m_data;

    /// The "virtual" size of the memory.
    size_t m_size = 0;

    /// The size of allocated memory. The initialization value is the initial capacity.
    size_t m_capacity = page_size;

    [[noreturn, gnu::cold]] static void handle_out_of_memory() noexcept { std::terminate(); }

    void allocate_capacity() noexcept
    {
        m_data.reset(static_cast<uint8_t*>(std::realloc(m_data.release(), m_capacity)));
        if (!m_data) [[unlikely]]
            handle_out_of_memory();
    }

public:
    /// Creates Memory object with initial capacity allocation.
    Memory() noexcept { allocate_capacity(); }

    uint8_t& operator[](size_t index) noexcept { return m_data[index]; }

    [[nodiscard]] size_t size() const noexcept { return m_size; }

    /// Grows the memory to the given size. The extent is filled with zeros.
    ///
    /// @param new_size  New memory size. Must be larger than the current size and multiple of 32.
    void grow(size_t new_size) noexcept
    {
        // Restriction for future changes. EVM always has memory size as multiple of 32 bytes.
        INTX_REQUIRE(new_size % 32 == 0);

        // Allow only growing memory. Include hint for optimizing compiler.
        INTX_REQUIRE(new_size > m_size);

        if (new_size > m_capacity)
        {
            m_capacity *= 2;  // Double the capacity.

            if (m_capacity < new_size)  // If not enough.
            {
                // Set capacity to required size rounded to multiple of page_size.
                m_capacity = ((new_size + (page_size - 1)) / page_size) * page_size;
            }

            allocate_capacity();
        }
        std::memset(&m_data[m_size], 0, new_size - m_size);
        m_size = new_size;
    }

    /// Virtually clears the memory by setting its size to 0. The capacity stays unchanged.
    void clear() noexcept { m_size = 0; }
};

/// EIP-8037 tx-wide reservoir pointer, propagated to every sub-frame's
/// ExecutionState. Callers set this before top-level execution and clear it
/// on tx exit. Single-thread-per-tx assumed.
inline thread_local int64_t* g_tx_state_gas_reservoir = nullptr;

/// EIP-8037: the child frame's `state_gas_from_gas_left` published on return
/// so the parent frame can merge it into its own on child SUCCESS. The parent
/// reads and consumes this immediately after `host.call` returns; the child
/// frame's own refill/reset semantics run inside baseline::execute before it
/// is published.
inline thread_local int64_t g_last_frame_state_gas_from_gas_left = 0;

/// EIP-8037: the child frame's reservoir draws published on SUCCESS so the
/// parent absorbs them into its own frame counter (refunded if the parent
/// itself later reverts/halts). Host-side call-boundary charges (CREATE /
/// CALL-to-new account) are added here too so they roll up the same way.
inline thread_local int64_t g_last_frame_state_gas_from_reservoir = 0;

/// EIP-8037: the child frame's signed net state usage, published on SUCCESS
/// for the parent merge (zero on failure — the frame rolled itself back).
inline thread_local int64_t g_last_frame_state_gas_used_net = 0;

/// EIP-8037: state usage charged outside any interpreter frame (top-level
/// transfer-to-new intercept). Reset per transaction.
inline thread_local int64_t g_tx_state_gas_used_extra = 0;

/// EIP-8037: tx-level state refunds (per-authorization refills, failed
/// create-tx NEW_ACCOUNT) — subtracted from the block state dimension.
/// Reset per transaction.
inline thread_local int64_t g_tx_state_refund_tx_level = 0;

/// EIP-8037: the top frame's final signed state usage, captured by the
/// transaction driver after the top-level call returns.
inline thread_local int64_t g_tx_top_frame_state_used = 0;

/// EIP-8038 (Amsterdam): per-tx set of accounts whose leaf has already been
/// written — ACCOUNT_WRITE (8,000) is charged only on the first write to an
/// account within a transaction. Reset per transaction. Function-local
/// thread_local: AppleClang emits duplicate TLS init routines for non-trivial
/// inline thread_local namespace-scope variables.
inline std::unordered_set<evmc::address>& tx_written_accounts()
{
    static thread_local std::unordered_set<evmc::address> set;
    return set;
}

/// Returns true (and records it) if this is the first leaf write to addr in the tx.
inline bool tx_first_account_write(const evmc::address& addr)
{
    return tx_written_accounts().insert(addr).second;
}

/// EIP-8246/EIP-8038: accounts marked for destruction in this tx. A transfer
/// into a dead destructed account (zero balance) rebuilds a leaf that
/// finalization owns — no ACCOUNT_WRITE surcharge applies. Maintained by the
/// Host, reset per transaction.
inline std::unordered_set<evmc::address>& tx_destructed_accounts()
{
    static thread_local std::unordered_set<evmc::address> set;
    return set;
}

/// EIP-8037 + EIP-7778 tx-wide state-gas ledger, reset per transaction.
/// Block state dimension = charged - refilled_gas_left: reservoir-drawn
/// charges stay gross at block level even when refilled (no refunds in block
/// accounting), while refills credited back to gas_left never left the
/// regular dimension, so they are discounted.
/// User/net accounting excludes ALL refills (both destinations).
inline thread_local int64_t g_tx_state_gas_charged = 0;
inline thread_local int64_t g_tx_state_gas_refilled_gas_left = 0;
inline thread_local int64_t g_tx_state_gas_refilled_reservoir = 0;

/// EIP-8037: the transaction's intrinsic STATE gas (worst-case auth charges +
/// CREATE-target NEW_ACCOUNT), set by the caller before transition so the
/// reservoir split can separate it from the regular intrinsic portion.
inline thread_local int64_t g_tx_intrinsic_state_gas = 0;

/// EIP-7928: suppress the Host's BAL observer for accesses that must not be
/// recorded unless a subsequent gas check passes (delegate resolution).
inline thread_local bool g_suppress_bal_observer = false;

/// EIP-8037 (devnet-7): the CREATE/CREATE2 NEW_ACCOUNT charge is conditional
/// on the target's aliveness (EELS is_account_alive), so the interpreter
/// needs the would-be target address before dispatching. The tx driver
/// installs this hook; its implementation computes the address the same way
/// the Host's prepare_message will.
/// Sets *sender_nonce_maxed when the creating account's nonce is at the
/// EIP-2681 limit — a "light" failure checked before the target is accessed.
using CreateTargetAddressFn = evmc::address (*)(
    void* ctx, const evmc::address& sender, bool is_create2, const evmc::bytes32& salt,
    const uint8_t* initcode, size_t initcode_size, bool* sender_nonce_maxed) noexcept;
inline thread_local CreateTargetAddressFn g_create_target_address_fn = nullptr;
inline thread_local void* g_create_target_address_ctx = nullptr;

/// Generic execution state for generic instructions implementations.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
class ExecutionState
{
public:
    int64_t gas_refund = 0;
    Memory memory;
    const evmc_message* msg = nullptr;
    evmc::HostContext host;
    evmc_revision rev = {};
    bytes return_data;

    /// Reference to original EVM code.
    bytes_view original_code;

    evmc_status_code status = EVMC_SUCCESS;
    size_t output_offset = 0;
    size_t output_size = 0;

    /// EIP-8037 (Amsterdam): pointer to the shared per-tx state-gas reservoir.
    /// State-creating opcodes charge from *state_gas_reservoir first; if it
    /// drops to zero the remainder falls through to gas_left and increments
    /// state_gas_from_gas_left (frame-local). state_gas_from_reservoir tracks
    /// the frame's reservoir draws so they can be refunded when the frame
    /// reverts or halts (all its state creations are rolled back). Left as
    /// nullptr on pre-Amsterdam forks so no path change.
    int64_t* state_gas_reservoir = nullptr;
    int64_t state_gas_from_gas_left = 0;
    int64_t state_gas_from_reservoir = 0;
    /// Signed per-frame state-gas usage (EELS `evm.state_gas_used`): charges
    /// increment, refund credits decrement — may go negative when a frame
    /// receives credits for an ancestor's charges. On frame failure the pools
    /// are restored from `used - spilled` (negative values claw back credits).
    int64_t state_gas_used_net = 0;

private:
    evmc_tx_context m_tx = {};

public:
    /// Pointer to code analysis.
    /// This should be set and used internally by execute() function of a particular interpreter.
    union
    {
        const baseline::CodeAnalysis* baseline = nullptr;
        const advanced::AdvancedCodeAnalysis* advanced;
    } analysis{};

    /// Stack space allocation.
    ///
    /// This is the last field to make other fields' offsets of reasonable values.
    StackSpace stack_space;

    ExecutionState() noexcept = default;

    ExecutionState(const evmc_message& message, evmc_revision revision,
        const evmc_host_interface& host_interface, evmc_host_context* host_ctx,
        bytes_view _code) noexcept
      : msg{&message}, host{host_interface, host_ctx}, rev{revision}, original_code{_code}
    {}

    /// Resets the contents of the ExecutionState so that it could be reused.
    void reset(const evmc_message& message, evmc_revision revision,
        const evmc_host_interface& host_interface, evmc_host_context* host_ctx,
        bytes_view _code) noexcept
    {
        gas_refund = 0;
        memory.clear();
        msg = &message;
        host = {host_interface, host_ctx};
        rev = revision;
        return_data.clear();
        original_code = _code;
        status = EVMC_SUCCESS;
        output_offset = 0;
        output_size = 0;
        state_gas_reservoir = g_tx_state_gas_reservoir;
        state_gas_from_gas_left = 0;
        state_gas_from_reservoir = 0;
        state_gas_used_net = 0;
        m_tx = {};
    }

    [[nodiscard]] bool in_static_mode() const { return (msg->flags & EVMC_STATIC) != 0; }

    const evmc_tx_context& get_tx_context() noexcept
    {
        if (INTX_UNLIKELY(m_tx.block_timestamp == 0))
            m_tx = host.get_tx_context();
        return m_tx;
    }
};
}  // namespace evmone

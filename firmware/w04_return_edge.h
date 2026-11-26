/** @file w04_return_edge.h Bounded x64 return check for the fixed W04 boot fixture. */
#ifndef HV_W04_RETURN_EDGE_H
#define HV_W04_RETURN_EDGE_H

/**
 * @brief Check a single observed x64 plain near RET against its original call frame.
 * AMD64 APM Vol. 3, RET (Near): the unprefixed 64-bit near return pops an
 * eight-byte guest virtual PC from the guest stack; RET imm16 is not admitted.
 * @pre The caller tracks an active invocation, owns and bounds-checks the frame,
 *      and samples the state immediately around the same natively executed instruction.
 * @param frame_rsp Original address of the invocation's stacked return address.
 * @param return_pc Original full-width caller continuation.
 * @param rsp_before RSP immediately before the instruction.
 * @param stacked_pc Full 64-bit word read at rsp_before before execution.
 * @param plain_near_ret Nonzero only for an observed plain near RET (opcode C3).
 * @param rip_after RIP immediately after the instruction.
 * @param rsp_after RSP immediately after the instruction.
 * @return Nonzero only for a matching transfer and a non-wrapping eight-byte pop.
 */
static inline int w04_handle_return_edge(unsigned long long frame_rsp, unsigned long long return_pc,
                                         unsigned long long rsp_before, unsigned long long stacked_pc,
                                         int plain_near_ret, unsigned long long rip_after,
                                         unsigned long long rsp_after)
{
    return plain_near_ret && rsp_before == frame_rsp && stacked_pc == return_pc &&
           rip_after == return_pc && rsp_after >= 8 && rsp_after - 8 == frame_rsp;
}

#endif

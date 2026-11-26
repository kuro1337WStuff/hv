/**
 * @file
 * @brief Host-only controls for the W04 return-edge decision.
 * Synthetic observations distinguish an original-frame RET from another transfer
 * with identical terminal registers. They do not establish frame ownership, decode
 * an instruction, execute a guest, or demonstrate a firmware service return.
 */
#include <stdio.h>
#include <stdlib.h>
#include "../../firmware/w04_return_edge.h"

int main(void)
{
    const unsigned long long frame = 0x0000000180001008ULL;
    const unsigned long long pc = 0x0000000240001234ULL;
    const unsigned long long high_bit = 1ULL << 32;
    const unsigned long long last = 0xffffffffffffffffULL;
    const struct {
        const char *name;
        unsigned long long original_rsp, original_pc, before, word;
        int ret;
        unsigned long long after_pc, after_rsp;
        int expected;
    } cases[] = {
        {"original RET", frame, pc, frame, pc, 1, pc, frame + 8, 1},
        {"same RIP/RSP but not RET", frame, pc, frame, pc, 0, pc, frame + 8, 0},
        {"substituted frame", frame, pc, frame ^ high_bit, pc, 1, pc, frame + 8, 0},
        {"substituted stack word", frame, pc, frame, pc ^ high_bit, 1, pc, frame + 8, 0},
        {"different RIP, same low bits", frame, pc, frame, pc, 1, pc ^ high_bit, frame + 8, 0},
        {"different RSP, same low bits", frame, pc, frame, pc, 1, pc, (frame + 8) ^ high_bit, 0},
        {"wrong pop size", frame, pc, frame, pc, 1, pc, frame + 4, 0},
        {"last non-wrapping pop", last - 8, pc + 16, last - 8, pc + 16, 1, pc + 16, last, 1},
        {"first wrapping pop", last - 7, pc + 16, last - 7, pc + 16, 1, pc + 16, 0, 0}
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (w04_handle_return_edge(cases[i].original_rsp, cases[i].original_pc,
                                   cases[i].before, cases[i].word, cases[i].ret,
                                   cases[i].after_pc, cases[i].after_rsp) != cases[i].expected) {
            fprintf(stderr, "unexpected W04 return-edge decision: %s\n", cases[i].name);
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}

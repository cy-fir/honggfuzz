#include "common.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(_HF_ARCH_LINUX)
#include <asm/prctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif                          // defined(_HF_ARCH_LINUX)

#include "util.h"

static feedback_t *feedback;
static uint32_t my_thread_no = 0;

__attribute__ ((weak))
uintptr_t __sanitizer_get_total_unique_coverage();

/* Fall-back mode, just map the buffer to avoid SIGSEGV in __cyg_profile_func_enter */
static void mapBBFallback(void)
{
    feedback =
        mmap(NULL, sizeof(feedback_t), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    feedback->pidFeedback[my_thread_no] = 0U;
    feedback->maxFeedback[my_thread_no] = 0U;
}

__attribute__ ((constructor))
static void mapBB(void)
{
    char *my_thread_no_str = getenv(_HF_THREAD_NO_ENV);
    if (my_thread_no_str == NULL) {
        mapBBFallback();
        return;
    }
    my_thread_no = atoi(my_thread_no_str);

    if (my_thread_no >= _HF_THREAD_MAX) {
        fprintf(stderr, "my_thread_no > _HF_THREAD_MAX (%" PRIu32 " > %d)\n", my_thread_no,
                _HF_THREAD_MAX);
        _exit(1);
    }
    struct stat st;
    if (fstat(_HF_BITMAP_FD, &st) == -1) {
        mapBBFallback();
        return;
    }
    if (st.st_size != sizeof(feedback_t)) {
        fprintf(stderr, "st.size != sizeof(feedback_t) (%zu != %zu)\n", (size_t) st.st_size,
                sizeof(feedback_t));
        _exit(1);
    }
    if ((feedback =
         mmap(NULL, sizeof(feedback_t), PROT_READ | PROT_WRITE, MAP_SHARED, _HF_BITMAP_FD,
              0)) == MAP_FAILED) {
        fprintf(stderr, "mmap: %s\n", strerror(errno));
        _exit(1);
    }
    feedback->pidFeedback[my_thread_no] = 0U;
    if (__sanitizer_get_total_unique_coverage == NULL) {
        feedback->maxFeedback[my_thread_no] = 0U;
    } else {
        feedback->maxFeedback[my_thread_no] = (uint64_t) __sanitizer_get_total_unique_coverage();
    }
}

/*
 * -finstrument-functions
 */
void __cyg_profile_func_enter(void *func, void *caller)
{
    register size_t pos =
        (((uintptr_t) func << 12) | ((uintptr_t) caller & 0xFFF)) & _HF_PERF_BITMAP_MASK;
    register uint8_t prev = ATOMIC_BTS(feedback->bbMap, pos);
    if (!prev) {
        ATOMIC_PRE_INC_RELAXED(feedback->pidFeedback[my_thread_no]);
    }
}

void __cyg_profile_func_exit(void *func UNUSED, void *caller UNUSED)
{
    return;
}

/*
 * -fsanitize=<address|memory|leak|undefined> -fsanitize-coverage=trace-pc,indirect-calls
 */
void __sanitizer_cov_trace_pc(void)
{
    register uintptr_t ret = (uintptr_t) __builtin_return_address(0) & _HF_PERF_BITMAP_MASK;
    register uint8_t prev = ATOMIC_BTS(feedback->bbMap, ret);
    if (!prev) {
        ATOMIC_PRE_INC_RELAXED(feedback->pidFeedback[my_thread_no]);
    }
}

void __sanitizer_cov_trace_pc_indir(void *callee)
{
    register size_t pos =
        (((uintptr_t) callee << 12) | ((uintptr_t) __builtin_return_address(0) & 0xFFF)) &
        _HF_PERF_BITMAP_MASK;
    register uint8_t prev = ATOMIC_BTS(feedback->bbMap, pos);
    if (!prev) {
        ATOMIC_PRE_INC_RELAXED(feedback->pidFeedback[my_thread_no]);
    }
}

static inline void incGs(unsigned long val)
{
#if defined(__x86_64__) && defined(_HF_ARCH_LINUX)
    if (val > 64) {
        fprintf(stderr, "VAL > 64: %lu\n", val);
        exit(1);
    }

    unsigned long gs;
    syscall(__NR_arch_prctl, ARCH_GET_GS, &gs);
    gs += val;
    syscall(__NR_arch_prctl, ARCH_SET_GS, gs);
#endif
}

void __sanitizer_cov_trace_cmp1(uint8_t Arg1, uint8_t Arg2)
{
    incGs(8U - __builtin_popcount(Arg1 ^ Arg2));
}

void __sanitizer_cov_trace_cmp2(uint16_t Arg1, uint16_t Arg2)
{
    incGs(16U - __builtin_popcount(Arg1 ^ Arg2));
}

void __sanitizer_cov_trace_cmp4(uint32_t Arg1, uint32_t Arg2)
{
    incGs(32U - __builtin_popcount(Arg1 ^ Arg2));
}

void __sanitizer_cov_trace_cmp8(uint64_t Arg1, uint64_t Arg2)
{
    incGs(64U - __builtin_popcountll(Arg1 ^ Arg2));
}

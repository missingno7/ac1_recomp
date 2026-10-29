#include "ac1_runtime.h"
#include "guest_clock.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: ac1_startup <AssassinsCreed_Dx9.exe>\n");
        return 2;
    }
    /* Redirected stderr is block-buffered (unbuffered made traced runs
     * unusably slow); the direct-memory fault handler flushes it. */
    uint64_t start = guest_monotonic_ns();
    int result = ac1_run(argv[1]);
    fprintf(stderr, "AC1 host run elapsed: %.3f ms (including EXE verification/loading)\n",
            (guest_monotonic_ns() - start) / 1000000.0);
    return result;
}

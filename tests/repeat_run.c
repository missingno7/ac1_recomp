#include "ac1_runtime.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: ac1_repeat_run <AssassinsCreed_Dx9.exe>\n");
        return 2;
    }

    int first_result = ac1_run(argv[1]);
    char first_error[512];
    snprintf(first_error, sizeof(first_error), "%s", ac1_last_error());

    int second_result = ac1_run(argv[1]);
    const char *second_error = ac1_last_error();
    printf("same-process repeat: first=%d second=%d same_result=%s same_boundary=%s\n",
           first_result, second_result,
           first_result == second_result ? "yes" : "no",
           !strcmp(first_error, second_error) ? "yes" : "no");
    if ((first_result != 0 && first_result != 8) ||
        first_result != second_result || strcmp(first_error, second_error)) {
        fprintf(stderr, "first boundary: %s\nsecond boundary: %s\n",
                first_error, second_error);
        return 1;
    }
    return 0;
}

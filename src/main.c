#include <stdio.h>
#include "sfmm.h"

int main(int argc, char const *argv[]) {
    double* ptr = sf_malloc(sizeof(double));

    *ptr = 1.3;
    sf_show_heap();

    printf("%f\n", *ptr);

    sf_free(ptr);
    sf_show_heap();

    return EXIT_SUCCESS;
}

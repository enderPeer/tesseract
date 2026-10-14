#include "benes.h"

#include <stdlib.h>
#include <string.h>

int benes_dim(int round)
{
    return round < CUBE_DIMS ? round : 2 * CUBE_DIMS - 2 - round;
}

typedef struct {
    uint16_t p;  /* where the value is on the way in */
    uint16_t q;  /* where it must be on the way out */
} Elem;

/* Routes n values that agree in bits 0..d-1 of p and q through rounds d .. 16-d. The input round
   across d sends each value into one of two half-networks (its "colour", bit d of its position
   inside); the two values of an input pair, and the two values bound for an output pair, must get
   different colours. Following that rule around each cycle colours everything (the looping
   algorithm), then each half-network is routed the same way. */
static void route(Elem *e, int n, int d, uint8_t ctrl[BENES_ROUNDS][CUBE_SIZE])
{
    unsigned bit = 1u << d;
    int in_round = d, out_round = 2 * CUBE_DIMS - 2 - d;
    if (d == CUBE_DIMS - 1) {  /* the middle round: cross if the value must change sides */
        for (int i = 0; i < n; i++)
            ctrl[in_round][e[i].p] = ((e[i].p ^ e[i].q) & bit) != 0;
        return;
    }
    int by_p[CUBE_SIZE], by_q[CUBE_SIZE];
    signed char *colour = malloc((size_t)n);
    for (int i = 0; i < n; i++) {
        by_p[e[i].p] = i;
        by_q[e[i].q] = i;
        colour[i] = -1;
    }
    for (int start = 0; start < n; start++) {
        int cur = start;
        while (colour[cur] < 0) {
            colour[cur] = 0;                          /* this value takes half 0 ... */
            int other = by_q[e[cur].q ^ bit];         /* ... so its output partner takes half 1 */
            colour[other] = 1;
            cur = by_p[e[other].p ^ bit];             /* ... and that one's input partner half 0 */
        }
    }
    Elem *half[2];
    int count[2] = {0, 0};
    half[0] = malloc((size_t)(n / 2) * sizeof(Elem));
    half[1] = malloc((size_t)(n / 2) * sizeof(Elem));
    for (int i = 0; i < n; i++) {
        unsigned c = (unsigned)colour[i];
        uint16_t p = (uint16_t)((e[i].p & ~bit) | (c << d)), q = (uint16_t)((e[i].q & ~bit) | (c << d));
        ctrl[in_round][e[i].p] = p != e[i].p;   /* crosses on the way in */
        ctrl[out_round][q] = q != e[i].q;       /* crosses on the way out */
        half[c][count[c]].p = p;
        half[c][count[c]++].q = q;
    }
    route(half[0], count[0], d + 1, ctrl);
    route(half[1], count[1], d + 1, ctrl);
    free(half[0]);
    free(half[1]);
    free(colour);
}

void benes_route(const uint16_t *dest, uint8_t ctrl[BENES_ROUNDS][CUBE_SIZE])
{
    Elem *e = malloc(CUBE_SIZE * sizeof *e);
    for (int k = 0; k < CUBE_SIZE; k++) {
        e[k].p = (uint16_t)k;
        e[k].q = dest[k];
    }
    memset(ctrl, 0, BENES_ROUNDS * CUBE_SIZE);
    route(e, CUBE_SIZE, 0, ctrl);
    free(e);
}

void benes_apply(const uint8_t ctrl[BENES_ROUNDS][CUBE_SIZE], uint64_t *v)
{
    uint64_t next[CUBE_SIZE];
    for (int s = 0; s < BENES_ROUNDS; s++) {
        unsigned bit = 1u << benes_dim(s);
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            next[k] = ctrl[s][k] ? v[k ^ bit] : v[k];
        memcpy(v, next, sizeof next);
    }
}

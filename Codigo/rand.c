// rand.c
// dispositivo de E/S para retornar um numero aleatorio
// simulador de computador
// so24b

#include "rand.h"

#include <stdlib.h>
#include <time.h>
#include <assert.h>

struct rand_t {
  int rand;
};

rand_t *rand_cria(void)
{
    rand_t *self;
    self = malloc(sizeof(rand_t));
    assert(self != NULL);

    // Inicializa a semente do gerador de números aleatórios
    srand(time(NULL));

    return self;
}

void rand_destroi(rand_t *self)
{
    free(self);
}

err_t rand_leitura(void *disp, int id, int *pvalor)
{
    err_t err = ERR_OK;
    switch (id) {
        case 0:
            // Gera um número aleatório entre 1 e 100
            *pvalor = rand() % 100 + 1;
            break;
        default:
            err = ERR_END_INV;
    }
    return err;
}

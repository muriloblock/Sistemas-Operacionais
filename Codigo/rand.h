// rand.h
// Header file for the random number generator device
// simulador de computador
// so24b

#ifndef RAND_H
#define RAND_H

#include "err.h"

// Estrutura para o dispositivo de E/S de números aleatórios
typedef struct rand_t rand_t;

// Função para criar uma nova instância do dispositivo de números aleatórios
rand_t *rand_cria(void);

// Função para destruir a instância do dispositivo de números aleatórios
void rand_destroi(rand_t *self);

err_t rand_leitura(void *disp, int id, int *pvalor);

#endif // RAND_H

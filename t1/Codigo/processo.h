#ifndef PROCESSO_H
#define PROCESSO_H

typedef enum {
    KERNEL = 0,
    USUARIO = 1,
} modo_processo_t;

typedef enum {
    PARADO = 0,
    EXECUTANDO = 1,
    PRONTO = 2,
    BLOQUEADO = 3,
    FINALIZADO = 4
} estado_processo_t;

typedef enum {
    ESCRITA = 3, // Esperando dispositivo de saída
    LEITURA,     // Esperando outro processo
    ESPERA
} motivo_bloqueio_t;


typedef struct proc_metricas_t {
    int vezes_pronto;
    int vezes_executando;
    int vezes_bloqueado;

    int tempo_pronto;
    int tempo_executando;
    int tempo_bloqueado;
    int tempo_total;

    int preempcoes;

    double tempo_medio_de_resposta;
} proc_metricas_t;

typedef struct {
    int pid;
    int pc;
    int a;
    int x;
    int dispositivo_saida;
    int dispositivo_entrada;
    int pid_esperado;
    double prioridade;
    proc_metricas_t metricas;
    motivo_bloqueio_t motivo_bloqueio;
    estado_processo_t estado;
    modo_processo_t modo;
} processo_t;

// Declarações dos setters e getters

// pid
void proc_set_pid(processo_t *proc, int pid);
int proc_get_pid(const processo_t *proc);

// pc
void proc_set_pc(processo_t *proc, int pc);
int proc_get_pc(const processo_t *proc);

// registrador A
void proc_set_a(processo_t *proc, int a);
int proc_get_a(const processo_t *proc);

// registrador X
void proc_set_x(processo_t *proc, int x);
int proc_get_x(const processo_t *proc);

// dispositivo_saida
void proc_set_dispositivo_saida(processo_t *proc, int dispositivo_saida);
int proc_get_dispositivo_saida(const processo_t *proc);

// dispositivo_entrada
void proc_set_dispositivo_entrada(processo_t *proc, int dispositivo_entrada);
int proc_get_dispositivo_entrada(const processo_t *proc);

// pid_esperado
void proc_set_pid_esperado(processo_t *proc, int pid);
int proc_get_pid_esperado(const processo_t *proc);

int proc_get_dispositivo_saida_ok(const processo_t *proc);
int proc_get_dispositivo_entrada_ok(const processo_t *proc);

// prioridade
void proc_set_prioridade(processo_t *proc, double prioridade);
double proc_get_prioridade(const processo_t *proc);

// motivo_bloqueio
void proc_set_motivo_bloqueio(processo_t *proc, motivo_bloqueio_t motivo);
motivo_bloqueio_t proc_get_motivo_bloqueio(const processo_t *proc);

// estado
void proc_set_estado(processo_t *proc, estado_processo_t estado);
estado_processo_t proc_get_estado(const processo_t *proc);

// modo
void proc_set_modo(processo_t *proc, modo_processo_t modo);
modo_processo_t proc_get_modo(const processo_t *proc);

void proc_set_tempo_pronto(processo_t *proc, int tempo);
int proc_get_tempo_pronto(const processo_t *proc);

void proc_set_tempo_executando(processo_t *proc, int tempo);
int proc_get_tempo_executando(const processo_t *proc);

void proc_set_tempo_bloqueado(processo_t *proc, int tempo);
int proc_get_tempo_bloqueado(const processo_t *proc);

void proc_set_preempcoes(processo_t *proc, int preempcoes);
int proc_get_preempcoes(const processo_t *proc);

// Funções para acessar as métricas
int proc_get_tempo_total(const processo_t *proc);
float proc_get_tempo_medio_de_resposta(const processo_t *proc);
int proc_get_vezes_executando(const processo_t *proc);
int proc_get_vezes_pronto(const processo_t *proc);
int proc_get_vezes_bloqueado(const processo_t *proc);


#endif // PROCESSO_H

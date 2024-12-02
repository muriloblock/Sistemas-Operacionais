#include "processo.h"
#include <stddef.h>

// pid
void proc_set_pid(processo_t *proc, int pid) {
    proc->pid = pid;
}

int proc_get_pid(const processo_t *proc) {
    return proc->pid;
}

// pc
void proc_set_pc(processo_t *proc, int pc) {
    proc->pc = pc;
}

int proc_get_pc(const processo_t *proc) {
    return proc->pc;
}

// registrador A
void proc_set_a(processo_t *proc, int a) {
    proc->a = a;
}

int proc_get_a(const processo_t *proc) {
    return proc->a;
}

// registrador X
void proc_set_x(processo_t *proc, int x) {
    proc->x = x;
}

int proc_get_x(const processo_t *proc) {
    return proc->x;
}

// dispositivo_saida
void proc_set_dispositivo_saida(processo_t *proc, int dispositivo_saida) {
    proc->dispositivo_saida = dispositivo_saida;
}

int proc_get_dispositivo_saida(const processo_t *proc) {
    return proc->dispositivo_saida;
}

// pid_esperado
void proc_set_pid_esperado(processo_t *proc, int pid) {
    proc->pid_esperado = pid;
}

int proc_get_pid_esperado(const processo_t *proc) {
    return proc->pid_esperado;
}

// prioridade
void proc_set_prioridade(processo_t *proc, double prioridade) {
    proc->prioridade = prioridade;
}

double proc_get_prioridade(const processo_t *proc) {
    return proc->prioridade;
}

// motivo_bloqueio
void proc_set_motivo_bloqueio(processo_t *proc, motivo_bloqueio_t motivo) {
    proc->motivo_bloqueio = motivo;
}

motivo_bloqueio_t proc_get_motivo_bloqueio(const processo_t *proc) {
    return proc->motivo_bloqueio;
}

// estado
void proc_set_estado(processo_t *proc, estado_processo_t estado) {
    if(proc == NULL || proc->estado == estado) return;

    proc->estado = estado;

    switch (estado)
    {
        case EXECUTANDO:
            proc->metricas.vezes_executando++;
            break;

        case PRONTO:
            proc->metricas.vezes_pronto++;
            break;

        case BLOQUEADO:
            proc->metricas.vezes_bloqueado++;
            break;

        default:
            break;
    }
}

estado_processo_t proc_get_estado(const processo_t *proc) {
    return proc->estado;
}

// modo
void proc_set_modo(processo_t *proc, modo_processo_t modo) {
    proc->modo = modo;
}

modo_processo_t proc_get_modo(const processo_t *proc) {
    return proc->modo;
}

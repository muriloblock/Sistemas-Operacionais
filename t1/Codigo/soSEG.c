// so.c
// sistema operacional
// simulador de computador
// so24b

// INCLUDES {{{1
#include "so.h"
#include "dispositivos.h"
#include "irq.h"
#include "programa.h"
#include "instrucao.h"

#include <stdlib.h>
#include <stdbool.h>

// CONSTANTES E TIPOS {{{1
// intervalo entre interrupções do relógio
#define INTERVALO_INTERRUPCAO 50   // em instruções executadas
#define MAX_PROCESSOS 10
#define PID_NENHUM -1  // Valor que indica ausência de um processo em execução

// Enum para o modo do processador
typedef enum {
  KERNEL = 0,
  USUARIO = 1,
} modo_processo_t;

// Enum para o estado do processo
typedef enum {
  PARADO = 0,
  EXECUTANDO = 1,
  PRONTO = 2,
  BLOQUEADO = 3,
  FINALIZADO = 4
} estado_processo_t;  

// Estrutura do processo usando os enums para estado e modo
typedef struct {
  int pid;                   // Identificador do processo
  int pc;                    // Contador de programa
  int a;                     // Registrador A
  int x;                     // Registrador X
  int pid_esperado;          // PID do processo aguardado (caso bloqueado)
  estado_processo_t estado;  // Estado do processo
  modo_processo_t modo;      // Modo de operação do processo
} processo_t;

struct so_t {
  cpu_t *cpu;
  mem_t *mem;
  es_t *es;
  console_t *console;
  processo_t tabela_processos[MAX_PROCESSOS]; // Tabela de processos
  int processo_atual;                        // PID do processo em execução
  bool erro_interno;
};

// função de tratamento de interrupção (entrada no SO)
static int so_trata_interrupcao(void *argC, int reg_A);

// funções auxiliares
// carrega o programa contido no arquivo na memória do processador; retorna end. inicial
static int so_carrega_programa(so_t *self, char *nome_do_executavel);
// copia para str da memória do processador, até copiar um 0 (retorna true) ou tam bytes
static bool copia_str_da_mem(int tam, char str[tam], mem_t *mem, int ender);

// CRIAÇÃO {{{1

void so_configura_relogio(so_t *self)
{
  // Programa o relógio para gerar uma interrupção após INTERVALO_INTERRUPCAO
  if (es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO) != ERR_OK) {
    console_printf("SO: problema na programação do timer");
    self->erro_interno = true;
  }
}

void so_configura_tratador_interrupcao(so_t *self)
{
  // Define a função que será chamada quando a CPU executar uma instrução CHAMAC
  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);

  // Carrega o programa de tratamento de interrupção
  int ender = so_carrega_programa(self, "trata_int.maq");
  if (ender != IRQ_END_TRATADOR) {
    console_printf("SO: problema na carga do programa de tratamento de interrupção");
    self->erro_interno = true;
  }
}

void so_inicializa_tabela_processos(so_t *self)
{
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    self->tabela_processos[i].estado = PARADO;
    self->tabela_processos[i].modo = USUARIO;  // Assume-se que os processos começam no modo usuário
    self->tabela_processos[i].pid = -1;  // Nenhum processo tem PID inicial
    self->tabela_processos[i].pc = 0;    // Inicializa o contador de programa
    self->tabela_processos[i].a = 0;     // Inicializa o registrador A
    self->tabela_processos[i].x = 0;     // Inicializa o registrador X
    self->tabela_processos[i].pid_esperado = -1; // Nenhum processo aguardando
  }
}

so_t *so_cria(cpu_t *cpu, mem_t *mem, es_t *es, console_t *console)
{
  // Aloca memória para o sistema operacional
  so_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;

  // Inicializa os componentes do sistema operacional
  self->cpu = cpu;
  self->mem = mem;
  self->es = es;
  self->console = console;
  self->erro_interno = false;

  // Inicializa a tabela de processos
  so_inicializa_tabela_processos(self);

  // Define o tratador de interrupção
  so_configura_tratador_interrupcao(self);

  // Configura o relógio para interrupções periódicas
  so_configura_relogio(self);

  // Inicializa o PID do processo atual (sem processo em execução por enquanto)
  self->processo_atual = -1;

  return self;
}

void so_destroi(so_t *self)
{
  cpu_define_chamaC(self->cpu, NULL, NULL);
  free(self);
}


// TRATAMENTO DE INTERRUPÇÃO {{{1

// funções auxiliares para o tratamento de interrupção
static void so_salva_estado_da_cpu(so_t *self);
static void so_trata_irq(so_t *self, int irq);
static void so_trata_pendencias(so_t *self);
static void so_escalona(so_t *self);
static int so_despacha(so_t *self);

// função a ser chamada pela CPU quando executa a instrução CHAMAC, no tratador de
//   interrupção em assembly
// essa é a única forma de entrada no SO depois da inicialização
// na inicialização do SO, a CPU foi programada para chamar esta função para executar
//   a instrução CHAMAC
// a instrução CHAMAC só deve ser executada pelo tratador de interrupção
//
// o primeiro argumento é um ponteiro para o SO, o segundo é a identificação
//   da interrupção
// o valor retornado por esta função é colocado no registrador A, e pode ser
//   testado pelo código que está após o CHAMAC. No tratador de interrupção em
//   assembly esse valor é usado para decidir se a CPU deve retornar da interrupção
//   (e executar o código de usuário) ou executar PARA e ficar suspensa até receber
//   outra interrupção
static int so_trata_interrupcao(void *argC, int reg_A)
{
  so_t *self = argC;
  irq_t irq = reg_A;
  // esse print polui bastante, recomendo tirar quando estiver com mais confiança
  console_printf("SO: recebi IRQ %d (%s)", irq, irq_nome(irq));
  // salva o estado da cpu no descritor do processo que foi interrompido
  so_salva_estado_da_cpu(self);
  // faz o atendimento da interrupção
  so_trata_irq(self, irq);
  // faz o processamento independente da interrupção
  so_trata_pendencias(self);
  // escolhe o próximo processo a executar
  so_escalona(self);
  // recupera o estado do processo escolhido
  return so_despacha(self);
}

static void so_salva_estado_da_cpu(so_t *self)
{
  // Verifica se há um processo corrente
  if (self->processo_atual == -1) {
    return;  // Se não houver processo corrente, não faz nada
  }

  // Recupera o processo corrente
  processo_t *proc = &self->tabela_processos[self->processo_atual];

  mem_le(self->mem, IRQ_END_PC, &proc->pc);
  mem_le(self->mem, IRQ_END_modo, (int*)&proc->modo);
  mem_le(self->mem, IRQ_END_A, &proc->a);
  mem_le(self->mem, IRQ_END_X, &proc->x);

  console_printf("SO: Estado do processo %d salvo. PC: %d, A: %d, X: %d, Modo: %d\n", 
                 proc->pid, proc->pc, proc->a, proc->x, proc->modo);
}

static void so_trata_pendencias(so_t *self)
{
    // Itera sobre todos os processos na tabela
    for (int i = 0; i < MAX_PROCESSOS; i++) {
        processo_t *proc = &self->tabela_processos[i];

        // Se o processo estiver bloqueado e esperando por um recurso de E/S, tenta desbloquear
        if (proc->estado == BLOQUEADO && proc->pid_esperado == self->processo_atual) {
            // Aqui você pode adicionar a lógica para desbloquear o processo, 
            // por exemplo, ao verificar que o processo aguardado terminou sua execução.
            proc->estado = PRONTO;
            console_printf("SO: Processo %d desbloqueado.\n", proc->pid);
        }

        // Se o processo estiver pronto, então podemos mover ele para a fila de execução
        if (proc->estado == PRONTO) {
            console_printf("SO: Processo %d está pronto para execução.\n", proc->pid);
        }
    }
}

static void so_escalona(so_t *self)
{
    // Verifica se há um processo corrente, caso contrário, escolhe o primeiro processo pronto
    if (self->processo_atual == -1) {
        for (int i = 0; i < MAX_PROCESSOS; i++) {
            processo_t *proc = &self->tabela_processos[i];

            if (proc->estado == PRONTO) {
                // Encontra o primeiro processo pronto e o define como o processo corrente
                self->processo_atual = proc->pid;
                proc->estado = EXECUTANDO;  // Atualiza o estado do processo para "executando"
                console_printf("SO: Processo %d agora está em execução.\n", proc->pid);
                break;
            }
        }
    }
    else {
        processo_t *proc = &self->tabela_processos[self->processo_atual];

        // Caso o processo corrente não possa continuar, escolhe um novo processo
        if (proc->estado != EXECUTANDO) {
            for (int i = 0; i < MAX_PROCESSOS; i++) {
                processo_t *proc2 = &self->tabela_processos[i];

                if (proc2->estado == PRONTO) {
                    // Define o próximo processo pronto como o processo corrente
                    self->processo_atual = proc2->pid;
                    proc2->estado = EXECUTANDO;
                    console_printf("SO: Processo %d agora está em execução.\n", proc2->pid);
                    break;
                }
            }
        }
    }
}

static int so_despacha(so_t *self)
{
    // Verifica se há um processo corrente
    if (self->processo_atual == -1) {
        console_printf("SO: Nenhum processo em execução.\n");
        return 1;  // Retorna 1 indicando erro
    }

    // Recupera o processo corrente
    processo_t *proc = &self->tabela_processos[self->processo_atual];

    // Atualiza o estado do processo na memória
    mem_escreve(self->mem, IRQ_END_PC, proc->pc);            // Atualiza o PC do processo
    mem_escreve(self->mem, IRQ_END_modo, proc->modo); // Atualiza o modo do processo
    mem_escreve(self->mem, IRQ_END_A, proc->a);              // Atualiza o registrador A
    mem_escreve(self->mem, IRQ_END_X, proc->x);              // Atualiza o registrador X

    console_printf("SO: Processo %d despachado. PC: %d, A: %d, X: %d, Modo: %d\n", 
                   proc->pid, proc->pc, proc->a, proc->x, proc->modo);

    return 0;  // Retorna 0 indicando que o despache foi bem-sucedido
}

// TRATAMENTO DE UMA IRQ {{{1

// funções auxiliares para tratar cada tipo de interrupção
static void so_trata_irq_reset(so_t *self);
static void so_trata_irq_chamada_sistema(so_t *self);
static void so_trata_irq_err_cpu(so_t *self);
static void so_trata_irq_relogio(so_t *self);
static void so_trata_irq_desconhecida(so_t *self, int irq);

static void so_trata_irq(so_t *self, int irq)
{
  // verifica o tipo de interrupção que está acontecendo, e atende de acordo
  switch (irq) {
    case IRQ_RESET:
      so_trata_irq_reset(self);
      break;
    case IRQ_SISTEMA:
      so_trata_irq_chamada_sistema(self);
      break;
    case IRQ_ERR_CPU:
      so_trata_irq_err_cpu(self);
      break;
    case IRQ_RELOGIO:
      so_trata_irq_relogio(self);
      break;
    default:
      so_trata_irq_desconhecida(self, irq);
  }
}

int so_aloca_pid(so_t *self)
{
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    if (self->tabela_processos[i].pid == -1) {
      return i;  // Retorna o primeiro PID disponível
    }
  }
  return -1;  // Se não houver espaço na tabela, retorna -1
}

void so_inicializa_processo(so_t *self, int pid, int ender)
{
  self->tabela_processos[pid].pid = pid;
  self->tabela_processos[pid].pc = ender;        // Define o PC do processo como o endereço de carga
  self->tabela_processos[pid].a = 0;             // Registra o A zerado
  self->tabela_processos[pid].x = 0;             // Registra o X zerado
  self->tabela_processos[pid].estado = PRONTO;   // O processo está pronto para ser executado
  self->tabela_processos[pid].modo = USUARIO;    // O processo deve iniciar no modo usuário
}

static void so_trata_irq_reset(so_t *self)
{
  // 1. Carrega o programa 'init.maq' na memória
  int ender = so_carrega_programa(self, "init.maq");
  if (ender != 100) {
    console_printf("SO: problema na carga do programa inicial");
    self->erro_interno = true;
    return;
  }

  // 2. Cria e inicializa o processo 'init'
  int pid_init = so_aloca_pid(self);
  if (pid_init == -1) {
    console_printf("SO: tabela de processos cheia");
    self->erro_interno = true;
    return;
  }

  so_inicializa_processo(self, pid_init, ender);

  self->processo_atual = pid_init;
}

// interrupção gerada quando a CPU identifica um erro
static void so_trata_irq_err_cpu(so_t *self)
{
  // Ocorreu um erro interno na CPU
  // O erro está codificado em IRQ_END_erro
  // Em geral, causa a morte do processo que causou o erro
  // Ainda não temos processos, causa a parada da CPU
  int err_int;
  // t1: com suporte a processos, deveria pegar o valor do registrador erro
  //   no descritor do processo corrente, e reagir de acordo com esse erro
  //   (em geral, matando o processo)
  mem_le(self->mem, IRQ_END_erro, &err_int);
  err_t err = err_int;
  console_printf("SO: IRQ não tratada -- erro na CPU: %s", err_nome(err));
  self->erro_interno = true;
}

// interrupção gerada quando o timer expira
static void so_trata_irq_relogio(so_t *self)
{
  // rearma o interruptor do relógio e reinicializa o timer para a próxima interrupção
  err_t e1, e2;
  e1 = es_escreve(self->es, D_RELOGIO_INTERRUPCAO, 0); // desliga o sinalizador de interrupção
  e2 = es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO);
  if (e1 != ERR_OK || e2 != ERR_OK) {
    console_printf("SO: problema da reinicialização do timer");
    self->erro_interno = true;
  }
  // t1: deveria tratar a interrupção
  //   por exemplo, decrementa o quantum do processo corrente, quando se tem
  //   um escalonador com quantum
  //console_printf("SO: interrupção do relógio (não tratada)");
}

// foi gerada uma interrupção para a qual o SO não está preparado
static void so_trata_irq_desconhecida(so_t *self, int irq)
{
  console_printf("SO: não sei tratar IRQ %d (%s)", irq, irq_nome(irq));
  self->erro_interno = true;
}

// CHAMADAS DE SISTEMA {{{1

// funções auxiliares para cada chamada de sistema
static void so_chamada_le(so_t *self);
static void so_chamada_escr(so_t *self);
static void so_chamada_cria_proc(so_t *self);
static void so_chamada_mata_proc(so_t *self);
static void so_chamada_espera_proc(so_t *self);

static void so_trata_irq_chamada_sistema(so_t *self)
{
  // a identificação da chamada está no registrador A
  // t1: com processos, o reg A tá no descritor do processo corrente
  int id_chamada;
  if (mem_le(self->mem, IRQ_END_A, &id_chamada) != ERR_OK) {
    console_printf("SO: erro no acesso ao id da chamada de sistema");
    self->erro_interno = true;
    return;
  }
  console_printf("SO: chamada de sistema %d", id_chamada);
  switch (id_chamada) {
    case SO_LE:
      so_chamada_le(self);
      break;
    case SO_ESCR:
      so_chamada_escr(self);
      break;
    case SO_CRIA_PROC:
      so_chamada_cria_proc(self);
      break;
    case SO_MATA_PROC:
      so_chamada_mata_proc(self);
      break;
    case SO_ESPERA_PROC:
      so_chamada_espera_proc(self);
      break;
    default:
      console_printf("SO: chamada de sistema desconhecida (%d)", id_chamada);
      // t1: deveria matar o processo
      self->erro_interno = true;
  }
}

// implementação da chamada se sistema SO_LE
// faz a leitura de um dado da entrada corrente do processo, coloca o dado no reg A
static void so_chamada_le(so_t *self)
{
  // implementação com espera ocupada
  //   T1: deveria realizar a leitura somente se a entrada estiver disponível,
  //     senão, deveria bloquear o processo.
  //   no caso de bloqueio do processo, a leitura (e desbloqueio) deverá
  //     ser feita mais tarde, em tratamentos pendentes em outra interrupção,
  //     ou diretamente em uma interrupção específica do dispositivo, se for
  //     o caso
  // implementação lendo direto do terminal A
  //   T1: deveria usar dispositivo de entrada corrente do processo
  for (;;) {
    int estado;
    if (es_le(self->es, D_TERM_A_TECLADO_OK, &estado) != ERR_OK) {
      console_printf("SO: problema no acesso ao estado do teclado");
      self->erro_interno = true;
      return;
    }
    if (estado != 0) break;
    // como não está saindo do SO, a unidade de controle não está executando seu laço.
    // esta gambiarra faz pelo menos a console ser atualizada
    // T1: com a implementação de bloqueio de processo, esta gambiarra não
    //   deve mais existir.
    console_tictac(self->console);
  }
  int dado;
  if (es_le(self->es, D_TERM_A_TECLADO, &dado) != ERR_OK) {
    console_printf("SO: problema no acesso ao teclado");
    self->erro_interno = true;
    return;
  }
  // escreve no reg A do processador
  // (na verdade, na posição onde o processador vai pegar o A quando retornar da int)
  // T1: se houvesse processo, deveria escrever no reg A do processo
  // T1: o acesso só deve ser feito nesse momento se for possível; se não, o processo
  //   é bloqueado, e o acesso só deve ser feito mais tarde (e o processo desbloqueado)
  mem_escreve(self->mem, IRQ_END_A, dado);
}

// implementação da chamada se sistema SO_ESCR
// escreve o valor do reg X na saída corrente do processo
static void so_chamada_escr(so_t *self)
{
  // implementação com espera ocupada
  //   T1: deveria bloquear o processo se dispositivo ocupado
  // implementação escrevendo direto do terminal A
  //   T1: deveria usar o dispositivo de saída corrente do processo
  for (;;) {
    int estado;
    if (es_le(self->es, D_TERM_A_TELA_OK, &estado) != ERR_OK) {
      console_printf("SO: problema no acesso ao estado da tela");
      self->erro_interno = true;
      return;
    }
    if (estado != 0) break;
    // como não está saindo do SO, a unidade de controle não está executando seu laço.
    // esta gambiarra faz pelo menos a console ser atualizada
    // T1: não deve mais existir quando houver suporte a processos, porque o SO não poderá
    //   executar por muito tempo, permitindo a execução do laço da unidade de controle
    console_tictac(self->console);
  }
  int dado;
  // está lendo o valor de X e escrevendo o de A direto onde o processador colocou/vai pegar
  // T1: deveria usar os registradores do processo que está realizando a E/S
  // T1: caso o processo tenha sido bloqueado, esse acesso deve ser realizado em outra execução
  //   do SO, quando ele verificar que esse acesso já pode ser feito.
  mem_le(self->mem, IRQ_END_X, &dado);
  if (es_escreve(self->es, D_TERM_A_TELA, dado) != ERR_OK) {
    console_printf("SO: problema no acesso à tela");
    self->erro_interno = true;
    return;
  }
  mem_escreve(self->mem, IRQ_END_A, 0);
}

// implementação da chamada se sistema SO_CRIA_PROC
// cria um processo
static void so_chamada_cria_proc(so_t *self)
{
  // ainda sem suporte a processos, carrega programa e passa a executar ele
  // quem chamou o sistema não vai mais ser executado, coitado!
  // T1: deveria criar um novo processo

  // em X está o endereço onde está o nome do arquivo
  int ender_proc;
  // t1: deveria ler o X do descritor do processo criador
  if (mem_le(self->mem, IRQ_END_X, &ender_proc) == ERR_OK) {
    char nome[100];
    if (copia_str_da_mem(100, nome, self->mem, ender_proc)) {
      int ender_carga = so_carrega_programa(self, nome);
      if (ender_carga > 0) {
        // t1: deveria escrever no PC do descritor do processo criado
        mem_escreve(self->mem, IRQ_END_PC, ender_carga);
        return;
      } // else?
    }
  }
  // deveria escrever -1 (se erro) ou o PID do processo criado (se OK) no reg A
  //   do processo que pediu a criação
  mem_escreve(self->mem, IRQ_END_A, -1);
}

// implementação da chamada se sistema SO_MATA_PROC
// mata o processo com pid X (ou o processo corrente se X é 0)
static void so_chamada_mata_proc(so_t *self)
{
  // T1: deveria matar um processo
  // ainda sem suporte a processos, retorna erro -1
  console_printf("SO: SO_MATA_PROC não implementada");
  mem_escreve(self->mem, IRQ_END_A, -1);
}

// implementação da chamada se sistema SO_ESPERA_PROC
// espera o fim do processo com pid X
static void so_chamada_espera_proc(so_t *self)
{
  // T1: deveria bloquear o processo se for o caso (e desbloquear na morte do esperado)
  // ainda sem suporte a processos, retorna erro -1
  console_printf("SO: SO_ESPERA_PROC não implementada");
  mem_escreve(self->mem, IRQ_END_A, -1);
}

// CARGA DE PROGRAMA {{{1

// carrega o programa na memória
// retorna o endereço de carga ou -1
static int so_carrega_programa(so_t *self, char *nome_do_executavel)
{
  // programa para executar na nossa CPU
  programa_t *prog = prog_cria(nome_do_executavel);
  if (prog == NULL) {
    console_printf("Erro na leitura do programa '%s'\n", nome_do_executavel);
    return -1;
  }

  int end_ini = prog_end_carga(prog);
  int end_fim = end_ini + prog_tamanho(prog);

  for (int end = end_ini; end < end_fim; end++) {
    if (mem_escreve(self->mem, end, prog_dado(prog, end)) != ERR_OK) {
      console_printf("Erro na carga da memória, endereco %d\n", end);
      return -1;
    }
  }

  prog_destroi(prog);
  console_printf("SO: carga de '%s' em %d-%d", nome_do_executavel, end_ini, end_fim);
  return end_ini;
}

// ACESSO À MEMÓRIA DOS PROCESSOS {{{1

// copia uma string da memória do simulador para o vetor str.
// retorna false se erro (string maior que vetor, valor não char na memória,
//   erro de acesso à memória)
// T1: deveria verificar se a memória pertence ao processo
static bool copia_str_da_mem(int tam, char str[tam], mem_t *mem, int ender)
{
  for (int indice_str = 0; indice_str < tam; indice_str++) {
    int caractere;
    if (mem_le(mem, ender + indice_str, &caractere) != ERR_OK) {
      return false;
    }
    if (caractere < 0 || caractere > 255) {
      return false;
    }
    str[indice_str] = caractere;
    if (caractere == 0) {
      return true;
    }
  }
  // estourou o tamanho de str
  return false;
}

// vim: foldmethod=marker

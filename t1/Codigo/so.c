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
#include "processo.h"

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>

#define INTERVALO_INTERRUPCAO 50
#define INTERVALO_QUANTUM     10
#define MAX_PROCESSOS         10
#define PID_NENHUM            -1

typedef enum {
  ESCALONADOR_NORMAL,
  ESCALONADOR_ROUND_ROBIN,
  ESCALONADOR_ROUND_ROBIN_PRIORIDADE
} escalonador_t;

typedef struct no {
  processo_t *processo;
  struct no *proximo;
  struct no *anterior;
} no_t;

typedef struct {
  no_t *inicio;
  no_t *fim;
} fila_t;

struct so_t {
  cpu_t *cpu;
  mem_t *mem;
  es_t *es;
  console_t *console;
  processo_t tabela_processos[MAX_PROCESSOS];
  processo_t *processo_corrente;
  fila_t *fila_processos;

  escalonador_t escalonador;

  int quantidade_processos;
  int quantum;
  int relogio;
  int contador_pid;
  bool erro_interno;

  int ultimo_relogio;
  int tempo_execucao;
  int tempo_ocioso;
  int preempcoes_totais;
  int *interrupcoes;
};

void atualiza_metricas(so_t *self, int irq)
{
  self->interrupcoes[irq]++;

  int ultimo_relogio = self->ultimo_relogio;
  if(es_le(self->es, D_RELOGIO_INSTRUCOES, &self->ultimo_relogio) != ERR_OK)
  {
    console_printf("Erro na leitura do relógio");
    exit(-1);
  }

  int elapsed_time = self->ultimo_relogio - ultimo_relogio;

  for (int i = 0; i < self->quantidade_processos; i++)
  {
    processo_t *proc = &self->tabela_processos[i];
    if(proc != NULL) {
      switch (proc_get_estado(proc)) {
        case EXECUTANDO:
          proc->metricas.tempo_executando += elapsed_time;
          break;
        case PRONTO:
          proc->metricas.tempo_pronto += elapsed_time;
          break;
        case BLOQUEADO:
          proc->metricas.tempo_bloqueado += elapsed_time;
          break;
        default:
          break;
      }
    }
  }
}

// função de tratamento de interrupção (entrada no SO)
static int so_trata_interrupcao(void *argC, int reg_A);

// funções auxiliares
// carrega o programa contido no arquivo na memória do processador; retorna end. inicial
static int so_carrega_programa(so_t *self, char *nome_do_executavel);
// copia para str da memória do processador, até copiar um 0 (retorna true) ou tam bytes
static bool copia_str_da_mem(int tam, char str[tam], mem_t *mem, int ender);

// CRIAÇÃO {{{1

// Inicializa a tabela de processos do SO
static void so_inicializa_tabela_processos(so_t *self) {
	for (int i = 0; i < MAX_PROCESSOS; i++) {
		self->tabela_processos[i].pid = PID_NENHUM;
		self->tabela_processos[i].pc = 0;
		self->tabela_processos[i].a = 0;
		self->tabela_processos[i].x = 0;
		self->tabela_processos[i].estado = PARADO;
		self->tabela_processos[i].modo = KERNEL;
		self->tabela_processos[i].pid_esperado = 0;
		self->tabela_processos[i].motivo_bloqueio = 0;
		self->tabela_processos[i].prioridade = 0;

		// Inicializa métricas
		self->tabela_processos[i].metricas.vezes_pronto = 0;
		self->tabela_processos[i].metricas.vezes_executando = 0;
		self->tabela_processos[i].metricas.vezes_bloqueado = 0;
		self->tabela_processos[i].metricas.tempo_pronto = 0;
		self->tabela_processos[i].metricas.tempo_executando = 0;
		self->tabela_processos[i].metricas.tempo_bloqueado = 0;
		self->tabela_processos[i].metricas.tempo_total = 0;
		self->tabela_processos[i].metricas.tempo_medio_de_resposta = 0;
		self->tabela_processos[i].metricas.preempcoes = 0;
	}
}


// Configura o timer do SO
static void so_configura_timer(so_t *self) {
  if (es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO) != ERR_OK) {
    console_printf("SO: problema na programação do timer\n");
    self->erro_interno = true;
  }
}

fila_t *cira_fila() {  // Alteração: retorno de ponteiro
  fila_t *self = (fila_t *)malloc(sizeof(fila_t));
  
  self->inicio = NULL;
  self->fim = NULL;
  
  return self;  // Aqui o retorno já está correto como ponteiro
}

void remove_fila (fila_t *self, processo_t *proc) {
  for (no_t *no = self->inicio; no != NULL; no = no->proximo) {
    if (no->processo != proc) {
      continue;
    }

    if (no->anterior != NULL) {
      no->anterior->proximo = no->proximo;
    } else {
      self->inicio = no->proximo;
    }

    if (no->proximo != NULL) {
      no->proximo->anterior = no->anterior;
    } else {
      self->fim = no->anterior;
    }

    free(no);

    break;
  }
}

so_t *so_cria(cpu_t *cpu, mem_t *mem, es_t *es, console_t *console) {
  so_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;

  // Inicializa os componentes do SO
  self->cpu = cpu;
  self->mem = mem;
  self->es = es;
  self->console = console;
  self->erro_interno = false;
  self->quantidade_processos = 0;
  self->contador_pid = 0;      // Inicializa o contador de PIDs
  self->processo_corrente = NULL; // Nenhum processo em execução inicialmente
  self->relogio = -1;
  self->quantum = 0;
  self->ultimo_relogio = 0;

  self->tempo_execucao = 0; //metricas
  self->tempo_ocioso = 0;
  self->preempcoes_totais = 0;
  self->escalonador = 0;
  self->interrupcoes = (int *)malloc(6 * sizeof(int));

  self->fila_processos = cira_fila();

  // Inicializa a tabela de processos e a fila de processos
  so_inicializa_tabela_processos(self);

  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);
  int ender = so_carrega_programa(self, "trata_int.maq");
  if (ender != IRQ_END_TRATADOR) {
    console_printf("SO: problema na carga do programa de tratamento de interrupção");
    self->erro_interno = true;
  }

  so_configura_timer(self);
  return self;
}

void so_destroi(so_t *self)
{
  cpu_define_chamaC(self->cpu, NULL, NULL);
  free(self);
}

static void so_salva_estado_da_cpu(so_t *self);
static void so_trata_irq(so_t *self, int irq);
static void so_trata_pendencias(so_t *self);
static void so_escalona(so_t *self);
static int so_despacha(so_t *self);

void so_imprime_metricas(so_t *self) {
    const char *nome_arquivo = "metricas_processos.txt";

    FILE *arquivo = fopen(nome_arquivo, "w");
    if (arquivo == NULL) {
        console_printf("Erro ao abrir o arquivo '%s' para escrita.\n", nome_arquivo);
        return;
    }

    fprintf(arquivo, "============================== MÉTRICAS DO SISTEMA ===============================\n\n");

    fprintf(arquivo, "  Escalonador                : %d\n\n", self->escalonador);

    fprintf(arquivo, "GERAL:\n");
    fprintf(arquivo, "  Processos criados          : %d\n", self->quantidade_processos);
    fprintf(arquivo, "  Tempo total de execução    : %d\n", self->tempo_execucao);
    fprintf(arquivo, "  Tempo total ocioso         : %d\n", self->tempo_ocioso);
    fprintf(arquivo, "  Número de preempções       : %d\n", self->preempcoes_totais);
    fprintf(arquivo, "\nINTERRUPÇÕES:\n");
    fprintf(arquivo, "  IRQ_RESET                  : %d\n", self->interrupcoes[IRQ_RESET]);
    fprintf(arquivo, "  IRQ_ERR_CPU                : %d\n", self->interrupcoes[IRQ_ERR_CPU]);
    fprintf(arquivo, "  IRQ_SISTEMA                : %d\n", self->interrupcoes[IRQ_SISTEMA]);
    fprintf(arquivo, "  IRQ_RELOGIO                : %d\n", self->interrupcoes[IRQ_RELOGIO]);
    fprintf(arquivo, "  IRQ_TECLADO                : %d\n", self->interrupcoes[IRQ_TECLADO]);
    fprintf(arquivo, "  IRQ_TELA                   : %d\n", self->interrupcoes[IRQ_TELA]);

    fprintf(arquivo, "\n============================ MÉTRICAS DOS PROCESSOS ============================\n\n");

    // Tabela de tempos
    fprintf(arquivo, "------------- TABELA DE TEMPOS -------------\n");
    fprintf(arquivo, "| PID | Tempo Exec. | Tempo Pronto | Tempo Bloq. | Tempo Retorno | Resp. Médio |\n");
    fprintf(arquivo, "|-----|-------------|--------------|-------------|---------------|-------------|\n");

    for (int i = 0; i < self->quantidade_processos; i++) {
        processo_t *proc = &self->tabela_processos[i];
        fprintf(arquivo,
            "| %-3d | %-11d | %-12d | %-11d | %-13d | %-11.2f |\n",
            proc_get_pid(proc),
            proc_get_tempo_executando(proc),
            proc_get_tempo_pronto(proc),
            proc_get_tempo_bloqueado(proc),
            proc_get_tempo_total(proc),
            proc_get_tempo_medio_de_resposta(proc));
    }

    fprintf(arquivo, "\n------------- TABELA DE VEZES -------------\n");
    fprintf(arquivo, "| PID | Execuções | Preempções | Vezes Pronto | Vezes Bloq. |\n");
    fprintf(arquivo, "|-----|-----------|------------|--------------|-------------|\n");

    // Tabela de vezes
    for (int i = 0; i < self->quantidade_processos; i++) {
        processo_t *proc = &self->tabela_processos[i];
        fprintf(arquivo,
            "| %-3d | %-9d | %-10d | %-12d | %-11d |\n",
            proc_get_pid(proc),
            proc_get_vezes_executando(proc),
            proc_get_preempcoes(proc),
            proc_get_vezes_pronto(proc),
            proc_get_vezes_bloqueado(proc));
    }

    fprintf(arquivo, "\n================================================================================\n");

    fclose(arquivo);

    console_printf("Métricas salvas no arquivo '%s'.\n", nome_arquivo);
}

static bool so_tem_trabalho(so_t *self)
{
  for (int i = 0; i < self->quantidade_processos; i++) {
    if (self->tabela_processos[i].estado != FINALIZADO) {
      return true;
    }
  }

  return false;
}

void calcula_metricas_final(so_t *self) {
  for (int i = 0; i < self->quantidade_processos; i++) {
    processo_t *proc = &self->tabela_processos[i];
    self->tempo_execucao += proc->metricas.tempo_executando;
    self->tempo_ocioso += proc->metricas.tempo_bloqueado;
    self->preempcoes_totais += proc->metricas.preempcoes;

    proc->metricas.tempo_total = proc->metricas.tempo_executando + proc->metricas.tempo_bloqueado + proc->metricas.tempo_pronto;
    proc->metricas.tempo_medio_de_resposta = (double)proc->metricas.tempo_pronto / proc->metricas.vezes_pronto;
  }
}

static int so_desliga(so_t *self)
{
  err_t e1, e2;
  e1 = es_escreve(self->es, D_RELOGIO_INTERRUPCAO, 0);
  e2 = es_escreve(self->es, D_RELOGIO_TIMER, 0);

  if (e1 != ERR_OK || e2 != ERR_OK) {
    console_printf("SO: problema de desarme do timer");
    self->erro_interno = true;
  }

  calcula_metricas_final(self);
  so_imprime_metricas(self);

  return 1;
}

static int so_trata_interrupcao(void *argC, int reg_A)
{
  so_t *self = argC;
  irq_t irq = reg_A;

  atualiza_metricas(self, irq);

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
  if (so_tem_trabalho(self)) {
    // recupera o estado do processo escolhido
    return so_despacha(self);
  } else {
    // para de executar o SO, desabilitando as interrupções de relógio
    return so_desliga(self);
  }
}

static int so_busca_indice_por_pid(so_t *self, int pid) {
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    if (self->tabela_processos[i].pid == pid) {
      return i; // Retorna o índice correspondente ao PID
    }
  }
  return -1;
}

void fila_insere(fila_t *self, processo_t *proc)
{
  no_t *no = (no_t *)malloc(sizeof(no_t));
  no->processo = proc;
  no->anterior = NULL;
  no->proximo = NULL;

  // Caso a fila esteja vazia
  if (self->inicio == NULL) {
    self->inicio = self->fim = no;
    return;
  }

  no_t *atual = self->inicio;

  // Percorre a fila até encontrar a posição correta
  while (atual != NULL && atual->processo->prioridade >= proc->prioridade) {
    atual = atual->proximo;
  }

  // Se o novo nó deve ser inserido no final
  if (atual == NULL) {
    no->anterior = self->fim;
    self->fim->proximo = no;
    self->fim = no;
  }
  // Se o novo nó deve ser inserido no início
  else if (atual == self->inicio) {
    no->proximo = self->inicio;
    self->inicio->anterior = no;
    self->inicio = no;
  }
  // Se o novo nó deve ser inserido no meio
  else {
    no->proximo = atual;
    no->anterior = atual->anterior;
    atual->anterior->proximo = no;
    atual->anterior = no;
  }
}


static void so_salva_estado_da_cpu(so_t *self) {
  if (self->processo_corrente == NULL || self->processo_corrente->estado != EXECUTANDO) {
    return;
  }

  processo_t *proc_atual = self->processo_corrente;
  mem_le(self->mem, IRQ_END_PC, &proc_atual->pc);
  mem_le(self->mem, IRQ_END_modo, (int *)&proc_atual->modo);
  mem_le(self->mem, IRQ_END_A, &proc_atual->a);
  mem_le(self->mem, IRQ_END_X, &proc_atual->x);
}

// Função para tratar bloqueio por escrita
static void trata_bloqueio_escrita(so_t *self, processo_t *proc) {
  int estado;
  es_le(self->es, proc_get_dispositivo_saida_ok(proc), &estado);
  if (estado != 0) {
    es_escreve(self->es, proc_get_dispositivo_saida(proc), proc_get_x(proc));
    proc_set_estado(proc, PRONTO);
    fila_insere(self->fila_processos, proc);
    proc_set_a(proc, 0);
  }
}

// Função para tratar bloqueio por leitura
static void trata_bloqueio_leitura(so_t *self, processo_t *proc) {
  int estado;
  es_le(self->es, proc_get_dispositivo_entrada_ok(proc), &estado);
  if (estado != 0) {
    int dado;
    es_le(self->es, proc_get_dispositivo_entrada(proc), &dado);
    proc_set_a(proc, dado);
    proc_set_estado(proc, PRONTO);
    fila_insere(self->fila_processos, proc);
  }
}


// Função para tratar bloqueio por espera
static void trata_bloqueio_espera(so_t *self, processo_t *proc) {
    for (int i = 0; i < MAX_PROCESSOS; i++) {
        processo_t *processo_esperado = &self->tabela_processos[i];
        if (processo_esperado->pid == proc->pid_esperado && processo_esperado->estado == FINALIZADO) {
            proc_set_estado(proc,PRONTO);
            fila_insere(self->fila_processos,proc);
            console_printf("SO: Processo PID=%d desbloqueado após término do processo PID=%d.\n", proc->pid, processo_esperado->pid);
            return;
        }
    }
    console_printf("SO: Processo PID=%d ainda aguardando o processo PID=%d finalizar.\n", proc->pid, proc->pid_esperado);
}

// Função principal para tratar o bloqueio do processo
static void so_trata_bloqueio(so_t *self, processo_t *proc) {
    // Verifica o motivo do bloqueio do processo
    switch (proc->motivo_bloqueio) {
        case ESCRITA:
            trata_bloqueio_escrita(self, proc);
            break;

        case LEITURA:
            trata_bloqueio_leitura(self, proc);
            break;

        case ESPERA:
            trata_bloqueio_espera(self, proc);
            break;

        default:
            console_printf("SO: Motivo de bloqueio desconhecido para o processo PID=%d.\n", proc->pid);
            break;
    }
}

static void so_trata_pendencias(so_t *self) {
  
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    processo_t *proc = &self->tabela_processos[i];

    if (proc->estado == BLOQUEADO) {
      so_trata_bloqueio(self, proc);
    }
  }
}

static void calcula_prioridade(so_t *self, processo_t *processo) {
    double t_exec = INTERVALO_QUANTUM - self->quantum;
    double prioridade = (processo->prioridade + (t_exec / INTERVALO_QUANTUM)) / 2;
    processo->prioridade = prioridade;
}

void fila_imprime(fila_t *self) {

    if (self == NULL || self->inicio == NULL) {
      console_printf("A fila está vazia ou não foi inicializada.\n");
      return;
    }

    no_t *no = self->inicio;  // Começa no primeiro nó da fila
    console_printf("=== TABELA DE PROCESSOS ===\n");
    while (no != NULL) {
        // Supondo que 'processo' tenha campos como 'pid' e 'prioridade'
        console_printf("Processo PID: %d, Prioridade: %f\n", no->processo->pid, no->processo->prioridade);
        
        // Avança para o próximo nó
        no = no->proximo;
    }
}

processo_t *proximo_processo(so_t *self) {
    if (self->fila_processos->inicio == NULL) {
        return NULL; // Nenhum processo na fila
    }

    // Retorna o processo no início da fila
    return self->fila_processos->inicio->processo;
}

static bool necessita_escalonar(so_t *self)
{
  if (self->processo_corrente == NULL) {
    return true;
  }

  if (self->processo_corrente->estado != EXECUTANDO) {
    return true;
  }

  if (self->quantum <= 0) {
    return true;
  }

  return false;
}

static void escalonador_normal(so_t *self) {
	// Se o processo corrente ainda está executando, mantém ele
	if (self->processo_corrente != NULL && proc_get_estado(self->processo_corrente) == EXECUTANDO) {
		return;
	}

	// Define o processo corrente como NULL inicialmente
	self->processo_corrente = NULL;

	// Busca o próximo processo pronto
	for (int i = 0; i < self->quantidade_processos; i++) {
		processo_t *proc = &self->tabela_processos[i]; // Obtem o processo da tabela
		if (proc != NULL && proc_get_estado(proc) == PRONTO) {
			self->processo_corrente = proc; // Define como o próximo processo corrente
			return;
		}
	}
}

static void escalonador_ROUND_ROBIN(so_t *self) {
  if(self->quantum == 0){

    remove_fila(self->fila_processos,self->processo_corrente);
    fila_insere(self->fila_processos, self->processo_corrente);

    self->processo_corrente->metricas.preempcoes++;
  }

  processo_t *proc = proximo_processo(self);

  if(proc == self->processo_corrente){
    self->quantum--;
    self->processo_corrente = proc;
    return;
  }else {
    self->quantum = INTERVALO_QUANTUM;
    self->processo_corrente = proc;
  }
}

static void escalonador_round_robin_PRIORIDADE(so_t *self) {
  fila_imprime(self->fila_processos);  // Imprime o conteúdo atual da fila

  processo_t *proc_prev = self->processo_corrente;

  if (!necessita_escalonar(self)) {
    return;
  }

  // Se um processo corrente existe, atualiza sua prioridade
  if (self->processo_corrente != NULL) {
    calcula_prioridade(self, self->processo_corrente);
  }

  // Escolhe o próximo processo
  self->processo_corrente = proximo_processo(self);

  // Se nenhum processo está pronto, define o quantum como 0 e retorna
  if (self->processo_corrente == NULL) {
    console_printf("SO: Nenhum processo pronto, aguardando interrupções.\n");
    self->quantum = 0; // Quantum zero indica que o SO está ocioso
    return;
  }

  // Se mudou o processo em execução, reseta o quantum
  if (self->processo_corrente != proc_prev) {
    self->quantum = INTERVALO_QUANTUM;
    self->processo_corrente->metricas.preempcoes++;
  }
}

static void so_escalona(so_t *self) {
    console_printf("=== TABELA DE PROCESSOS ===\n");
    for (int i = 0; i < self->quantidade_processos; i++) {
        processo_t *proc = &self->tabela_processos[i];
        console_printf("I=%d: PID=%d, PC=%d, A=%d, X=%d, ESTADO=%d, EXEC=%d, PRONT=%d, BLOQ=%d\n",
                       i, proc->pid, proc->pc, proc->a, proc->x, proc->estado, proc->metricas.tempo_executando,
                       proc->metricas.tempo_pronto, proc->pid_esperado);
    }

  switch (self->escalonador) {
		case ESCALONADOR_NORMAL:
			escalonador_normal(self);
			break;

		case ESCALONADOR_ROUND_ROBIN:
			escalonador_ROUND_ROBIN(self);
			break;

		case ESCALONADOR_ROUND_ROBIN_PRIORIDADE:
			escalonador_round_robin_PRIORIDADE(self);
			break;

		default:
			console_printf("SO: Escalonador desconhecido! Nenhuma ação será realizada.\n");
			break;
	  }
}

static int so_despacha(so_t *self) {
  if (self->processo_corrente == NULL) {
    console_printf("SO: Nenhum processo disponível para despachar, aguardando interrupções...\n");
    return 1; // Retorna indicando que não há processos para executar
  }

  processo_t *proc = self->processo_corrente;

  // Configura a CPU com os valores do processo corrente usando get para acessar os valores
  mem_escreve(self->mem, IRQ_END_PC,     proc_get_pc(proc));    // Configura o valor do PC
  mem_escreve(self->mem, IRQ_END_modo, proc_get_modo(proc));    // Configura o modo de operação
  mem_escreve(self->mem, IRQ_END_A,       proc_get_a(proc));    // Configura o registrador A
  mem_escreve(self->mem, IRQ_END_X,       proc_get_x(proc));    // Configura o registrador X

  if (self->erro_interno) {
    return 1;
  } else {
    // Define o estado como EXECUTANDO usando set
    proc_set_estado(proc, EXECUTANDO);
    return 0;
  }
}


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

// Função para configurar o novo processo
static void configura_novo_processo(processo_t *novo_proc, int pid, int ender_carga) {
  proc_set_pid(novo_proc, pid);
  proc_set_pc(novo_proc, ender_carga);
  proc_set_a(novo_proc, 0);
  proc_set_x(novo_proc, 0);
  proc_set_estado(novo_proc, PRONTO);
  proc_set_modo(novo_proc, USUARIO);
  proc_set_pid_esperado(novo_proc, 0);
  proc_set_prioridade(novo_proc, 0.5);

  // Inicializa as métricas utilizando os setters
  proc_set_tempo_pronto(novo_proc, 0);
  proc_set_tempo_executando(novo_proc, 0);
  proc_set_tempo_bloqueado(novo_proc, 0);
  proc_set_preempcoes(novo_proc, 0);
}

// Função para definir o dispositivo de saída com base no PID
static void define_dispositivos(processo_t *novo_proc) {
	switch (proc_get_pid(novo_proc) % 4) {
		case 0:
			proc_set_dispositivo_saida(novo_proc, D_TERM_A_TELA);
      proc_set_dispositivo_entrada(novo_proc, D_TERM_A_TECLADO);
			break;
		case 1:
			proc_set_dispositivo_saida(novo_proc, D_TERM_B_TELA);
      proc_set_dispositivo_entrada(novo_proc, D_TERM_B_TECLADO);

			break;
		case 2:
			proc_set_dispositivo_saida(novo_proc, D_TERM_C_TELA);
      proc_set_dispositivo_entrada(novo_proc, D_TERM_C_TECLADO);

			break;
		case 3:
			proc_set_dispositivo_saida(novo_proc, D_TERM_D_TELA);
      proc_set_dispositivo_entrada(novo_proc, D_TERM_D_TECLADO);
			break;
	}
}

// Interrupção gerada uma única vez, quando a CPU inicializa
static void so_trata_irq_reset(so_t *self) {
  self->quantidade_processos++;
  // Cria e inicializa o processo init
  processo_t *init_proc = &self->tabela_processos[0];
  int ender = so_carrega_programa(self, "init.maq");
  if (ender < 0) {
    console_printf("SO: problema na carga do programa inicial\n");
    self->erro_interno = true;
    return;
  }

  configura_novo_processo(init_proc, self->contador_pid++, ender);
  
  define_dispositivos(init_proc);

  fila_insere(self->fila_processos,init_proc);

  self->processo_corrente = init_proc;
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
  self->quantum--;
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

static void bloqueia_processo(so_t *self, motivo_bloqueio_t MOTIVO)
{
  remove_fila(self->fila_processos,self->processo_corrente);

  proc_set_estado      (self->processo_corrente, BLOQUEADO);
  proc_set_motivo_bloqueio(self->processo_corrente, MOTIVO);

  if (MOTIVO == ESPERA){
    proc_set_pid_esperado(self->processo_corrente, proc_get_x(self->processo_corrente));
  }
}

// Implementação da chamada de sistema SO_LE
// Faz a leitura de um dado da entrada corrente do processo, coloca o dado no reg A
static void so_chamada_le(so_t *self)
{
  int estado;
  es_le(self->es, proc_get_dispositivo_entrada(self->processo_corrente), &estado);

  if (estado != 0) {
    int dado;
    es_le(self->es, proc_get_dispositivo_entrada(self->processo_corrente) + 1, &dado);
    mem_escreve(self->mem, IRQ_END_A, dado);
  } else {
    bloqueia_processo(self, LEITURA);
  }
}


// Implementação da chamada de sistema SO_ESCR
// Escreve o valor do registrador X na saída corrente do processo
static void so_chamada_escr(so_t *self) {
  int estado;
  es_le(self->es, proc_get_dispositivo_saida_ok(self->processo_corrente), &estado);

  if (estado != 0) {
    es_escreve(self->es, proc_get_dispositivo_saida(self->processo_corrente), proc_get_x(self->processo_corrente));
    mem_escreve(self->mem, IRQ_END_A, 0);
  } else {
    bloqueia_processo(self, ESCRITA);
  }
}


// Função para ler o nome do processo da memória
static void le_nome_do_processso(so_t *self, int ender_proc, char *nome) {
  copia_str_da_mem(sizeof(nome), nome, self->mem, ender_proc);
}

// Função para encontrar um índice livre na tabela de processos
static int encontra_indice_livre(so_t *self) {
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    if (self->tabela_processos[i].pid == PID_NENHUM) {
      return i;
    }
  }
  return -1;  // Retorna -1 se não houver espaço livre
}

// Função principal da chamada de sistema SO_CRIA_PROC
static void so_chamada_cria_proc(so_t *self) {
  self->quantidade_processos++;

  char nome[100];
  le_nome_do_processso(self, self->processo_corrente->x, nome);  // Lê o nome do processo

  // Carrega o programa na memória
  int ender_carga = so_carrega_programa(self, nome);

  // Encontra um índice livre na tabela de processos
  int indice_livre = encontra_indice_livre(self);
  if (indice_livre == -1) {
    return;  // Não há espaço para criar um novo processo
  }

  // Cria e configura o novo processo
  processo_t *novo_proc = &self->tabela_processos[indice_livre];
  configura_novo_processo(novo_proc, self->contador_pid++, ender_carga);
  
  // Define o dispositivo de saída
  define_dispositivos(novo_proc);

  fila_insere(self->fila_processos,novo_proc);

  // Define o PID do novo processo no registrador A do processo corrente
  proc_set_a(self->processo_corrente,proc_get_pid(novo_proc));
}

// Implementação da chamada de sistema SO_MATA_PROC
// Mata o processo com PID X (ou o processo corrente se X for 0)
static void so_chamada_mata_proc(so_t *self) {
  processo_t *proc = self->processo_corrente;

  if (proc->x != 0) {
    int index = so_busca_indice_por_pid(self, proc_get_x(self->processo_corrente));
    proc = &self->tabela_processos[index];
  }
  proc_set_estado(proc,FINALIZADO);
  remove_fila(self->fila_processos,proc);
}

// Implementação da chamada de sistema SO_ESPERA_PROC
// Bloqueia o processo chamador até que o processo com PID X termine.
static void so_chamada_espera_proc(so_t *self) {
  bloqueia_processo(self, ESPERA);
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
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
    PRONTO = 2,       // Novo estado PRONTO
    BLOQUEADO = 3     // Novo estado BLOQUEADO
} estado_processo_t;

// Estrutura do processo usando os enums para estado e modo
typedef struct {
    int pid;                   // Identificador do processo
    int pc;                    // Contador de programa
    estado_processo_t estado;  // Estado do processo
    modo_processo_t modo;      // Modo de operação do processo
    int pid_esperado;          // PID do processo aguardado (caso bloqueado)
    // Outros registradores podem ser adicionados aqui
} processo_t;

struct so_t {
  cpu_t *cpu;
  mem_t *mem;
  es_t *es;
  console_t *console;
  processo_t tabela_processos[MAX_PROCESSOS]; // Tabela de processos
  int processo_atual;                        // PID do processo em execução
  bool erro_interno;
  // t1: tabela de processos, processo corrente, pendências, etc
};


// função de tratamento de interrupção (entrada no SO)
static int so_trata_interrupcao(void *argC, int reg_A);

// funções auxiliares
// carrega o programa contido no arquivo na memória do processador; retorna end. inicial
static int so_carrega_programa(so_t *self, char *nome_do_executavel);
// copia para str da memória do processador, até copiar um 0 (retorna true) ou tam bytes
static bool copia_str_da_mem(int tam, char str[tam], mem_t *mem, int ender);

// CRIAÇÃO {{{1

so_t *so_cria(cpu_t *cpu, mem_t *mem, es_t *es, console_t *console)
{
  so_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;

  self->cpu = cpu;
  self->mem = mem;
  self->es = es;
  self->console = console;
  self->erro_interno = false;
  self->processo_atual = PID_NENHUM; // Nenhum processo em execução no início

  // Inicializa a tabela de processos
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    self->tabela_processos[i].pid = PID_NENHUM;        // Nenhum processo
    self->tabela_processos[i].pc = 0;                  // PC inicial
    self->tabela_processos[i].estado = PARADO;         // Estado inicial
    self->tabela_processos[i].modo = USUARIO;          // Modo inicial padrão
    // Outros registradores podem ser inicializados aqui, se necessário
  }

  // Define o tratamento de interrupções
  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);

  // Carrega o programa de tratamento de interrupção
  int ender = so_carrega_programa(self, "trata_int.maq");
  if (ender != IRQ_END_TRATADOR) {
    console_printf("SO: problema na carga do programa de tratamento de interrupção");
    self->erro_interno = true;
  }

  // Programa o timer para gerar uma interrupção após INTERVALO_INTERRUPCAO
  if (es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO) != ERR_OK) {
    console_printf("SO: problema na programação do timer");
    self->erro_interno = true;
  }

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

// Função para salvar o estado da CPU no processo atual
static void salva_estado_cpu(so_t *self) {
    // Verifica se há um processo em execução
    if (self->processo_atual == PID_NENHUM) return;

    processo_t *processo = &self->tabela_processos[self->processo_atual];

    // Salvando os valores dos registradores da CPU nos campos correspondentes do processo
    mem_le(self->mem, IRQ_END_PC, &processo->pc);  // PC (Contador de Programa)

    // Salvando o modo de operação (usuário ou kernel) no processo atual
    mem_le(self->mem, IRQ_END_modo, (int*)&processo->modo);  // Modo de operação
}

// Implementação da função so_salva_estado_da_cpu que chama salva_estado_cpu para salvar o estado do processo atual
static void so_salva_estado_da_cpu(so_t *self) {
    salva_estado_cpu(self);
}


static void so_trata_pendencias(so_t *self)
{
  // t1: realiza ações que não são diretamente ligadas com a interrupção que
  //   está sendo atendida:
  // - E/S pendente
  // - desbloqueio de processos
  // - contabilidades
}

static void so_escalona(so_t *self) {
    // Verifica se há um processo em execução
    if (self->processo_atual != PID_NENHUM) {
        processo_t *processo_corrente = &self->tabela_processos[self->processo_atual];

        // Se o processo atual não pode continuar, procura um novo processo
        if (processo_corrente->estado != EXECUTANDO) {
            // Procura o próximo processo pronto para executar
            for (int i = 0; i < MAX_PROCESSOS; i++) {
                // Se encontramos um processo pronto, coloca ele como o processo corrente
                if (self->tabela_processos[i].estado == EXECUTANDO) {
                    self->processo_atual = self->tabela_processos[i].pid;
                    return;  // Encontrou e escalonou um processo, retorna
                }
            }
        }
    }
    
    // Caso não tenha nenhum processo em execução, ou todos estejam bloqueados, 
    // não altera o processo atual.
}


// Função para recuperar o estado da CPU a partir da tabela de processos
static void recupera_estado_cpu(so_t *self) {
    // Verifica se há um processo em execução
    if (self->processo_atual == PID_NENHUM) return;

    processo_t *processo = &self->tabela_processos[self->processo_atual];

    // Recuperando os valores dos registradores da tabela de processos e colocando na memória
    mem_escreve(self->mem, IRQ_END_PC, processo->pc);  // PC (Contador de Programa)
    
    // Passando o valor do modo para a memória
    mem_escreve(self->mem, IRQ_END_modo, (int)processo->modo);  // Modo de operação
}

static int so_despacha(so_t *self)
{
    // Se houver erro interno, retorna 1 (indicando erro)
    if (self->erro_interno) return 1;

    // Se houver um processo corrente, recupera seu estado e retorna 0
    if (self->processo_atual != PID_NENHUM) {
        // Chama a função para recuperar o estado da CPU a partir do processo atual
        recupera_estado_cpu(self);
        return 0;
    }

    // Se não houver processo corrente, retorna 1 (indicando erro ou ausência de processo)
    return 1;
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

static void so_trata_irq_reset(so_t *self)
{
  int ender = so_carrega_programa(self, "init.maq");
  if (ender != 100) {
    console_printf("SO: problema na carga do programa inicial");
    self->erro_interno = true;
    return;
  }

  // Configura o processo init na tabela de processos
  self->tabela_processos[0].pid = 0;         // Primeiro processo, com PID 0
  self->tabela_processos[0].pc = ender;      // Define o PC como o endereço de carga de init
  self->tabela_processos[0].estado = 1;      // Define o estado como executando
  self->tabela_processos[0].modo = usuario;  // Modo de operação do processo (usuário)
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
  console_printf("SO: interrupção do relógio (não tratada)");
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

// implementação da chamada de sistema SO_CRIA_PROC
// cria um novo processo e adiciona à tabela de processos
static void so_chamada_cria_proc(so_t *self)
{
    int ender_proc;
    if (mem_le(self->mem, IRQ_END_X, &ender_proc) == ERR_OK) {
        char nome[100];
        if (copia_str_da_mem(100, nome, self->mem, ender_proc)) {
            int ender_carga = so_carrega_programa(self, nome);
            if (ender_carga > 0) {
                // Encontra uma entrada livre na tabela de processos
                for (int i = 0; i < MAX_PROCESSOS; i++) {
                    if (self->tabela_processos[i].estado == PARADO) {
                        processo_t *novo_processo = &self->tabela_processos[i];
                        novo_processo->pid = i;
                        novo_processo->pc = ender_carga;
                        novo_processo->estado = EXECUTANDO;  // ou PRONTO, dependendo da lógica de escalonamento
                        novo_processo->modo = USUARIO;

                        // Retorna o PID do novo processo no registrador A
                        mem_escreve(self->mem, IRQ_END_A, novo_processo->pid);
                        return;
                    }
                }
            }
        }
    }
    // Caso ocorra algum erro, retorna -1
    mem_escreve(self->mem, IRQ_END_A, -1);
}

// Função auxiliar chamada quando um processo termina, para verificar e desbloquear processos esperando por ele
static void so_desbloqueia_esperando(so_t *self, int pid_terminado)
{
    for (int i = 0; i < MAX_PROCESSOS; i++) {
        if (self->tabela_processos[i].estado == BLOQUEADO &&
            self->tabela_processos[i].pid_esperado == pid_terminado) {
            self->tabela_processos[i].estado = PRONTO;  // Desbloqueia o processo
            self->tabela_processos[i].pid_esperado = -1;  // Limpa o pid esperado
        }
    }
}

static void so_chamada_mata_proc(so_t *self)
{
    int pid;
    if (mem_le(self->mem, IRQ_END_X, &pid) == ERR_OK) {
        // Se o PID for 0, significa o processo atual
        if (pid == 0) {
            pid = self->processo_atual;
        }

        // Verifica se o PID é válido
        if (pid >= 0 && pid < MAX_PROCESSOS && self->tabela_processos[pid].estado != PARADO) {
            self->tabela_processos[pid].estado = PARADO;  // Define o estado como PARADO ou TERMINADO
            self->tabela_processos[pid].pc = 0;  // Zera o PC para evitar reuso indevido

            // Se o processo atual foi terminado, define que nenhum processo está em execução
            if (pid == self->processo_atual) {
                self->processo_atual = PID_NENHUM;
            }

            // Desbloqueia processos em espera por esse PID
            so_desbloqueia_esperando(self, pid);

            // Retorna sucesso (0) no registrador A
            mem_escreve(self->mem, IRQ_END_A, 0);
            return;
        }
    }
    // Caso ocorra algum erro, retorna -1
    mem_escreve(self->mem, IRQ_END_A, -1);
}

// implementação da chamada de sistema SO_ESPERA_PROC
// espera o fim do processo com pid X
static void so_chamada_espera_proc(so_t *self)
{
    int pid_esperado;
    if (mem_le(self->mem, IRQ_END_X, &pid_esperado) != ERR_OK) {
        // Caso ocorra erro ao ler o PID, escreve -1 em A e retorna
        mem_escreve(self->mem, IRQ_END_A, -1);
        return;
    }

    // Se o pid esperado for zero, retorna erro, pois não é possível esperar o próprio processo
    if (pid_esperado == 0 || pid_esperado >= MAX_PROCESSOS || 
        self->tabela_processos[pid_esperado].estado == PARADO) {
        // Retorna imediatamente se o PID não existe ou já está terminado
        mem_escreve(self->mem, IRQ_END_A, 0);
        return;
    }

    // Verifica se o processo já terminou
    if (self->tabela_processos[pid_esperado].estado == PARADO) {
        mem_escreve(self->mem, IRQ_END_A, 0);  // Retorna sucesso imediato
        return;
    }

    // Caso contrário, bloqueia o processo atual
    self->tabela_processos[self->processo_atual].estado = BLOQUEADO;
    self->tabela_processos[self->processo_atual].pid_esperado = pid_esperado;

    // Escreve -1 no registrador A para indicar que o processo foi bloqueado
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

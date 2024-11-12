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
  int pid_processo_atual;                     // PID do processo em execução
  int index_processo_atual;                   // Index do processo atual na tabela
  int contador_pid;                           // Contador para gerar os PIDs, tipo variavel global
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

so_t *so_cria(cpu_t *cpu, mem_t *mem, es_t *es, console_t *console)
{
  // Aloca memória para o SO
  so_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;

  // Inicializa os ponteiros para os componentes do SO
  self->cpu = cpu;
  self->mem = mem;
  self->es = es;
  self->console = console;
  self->erro_interno = false;

  // Inicializa a tabela de processos
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    self->tabela_processos[i].pid = PID_NENHUM;  // Nenhum processo alocado inicialmente
    self->tabela_processos[i].estado = PARADO;   // Todos os processos começam como parados
    self->tabela_processos[i].pc = 0;             // Contador de programa inicial
    self->tabela_processos[i].a = 0;              // Inicializa o registrador A
    self->tabela_processos[i].x = 0;              // Inicializa o registrador X
    self->tabela_processos[i].pid_esperado = PID_NENHUM; // Sem processo aguardado inicialmente
    self->tabela_processos[i].modo = KERNEL;      // Todos os processos começam no modo kernel
  }

  // Inicializa o PID do processo atual e o contador de PID
  self->pid_processo_atual = PID_NENHUM;  // Nenhum processo está em execução inicialmente
  self->contador_pid = 10;                 // O contador de PID começa em 10 (não 0)
  self->index_processo_atual = -1;        // Nenhum processo está em execução inicialmente

  // Quando a CPU executar uma instrução CHAMAC, deve chamar a função
  // so_trata_interrupcao, com o primeiro argumento um ptr para o SO
  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);

  // Coloca o tratador de interrupção na memória
  // Quando a CPU aceita uma interrupção, passa para modo supervisor, 
  // salva seu estado a partir do endereço 0, e desvia para o endereço
  // IRQ_END_TRATADOR
  // Colocamos no endereço IRQ_END_TRATADOR o programa de tratamento
  // de interrupção (escrito em asm). Esse programa deve conter a 
  // instrução CHAMAC, que vai chamar so_trata_interrupcao
  int ender = so_carrega_programa(self, "trata_int.maq");
  if (ender != IRQ_END_TRATADOR) {
    console_printf("SO: problema na carga do programa de tratamento de interrupção");
    self->erro_interno = true;
  }

  // Programa o relógio para gerar uma interrupção após INTERVALO_INTERRUPCAO
  if (es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO) != ERR_OK) {
    console_printf("SO: problema na programação do timer");
    self->erro_interno = true;
  }

  // Retorna a estrutura do sistema operacional
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
    if (self->index_processo_atual == -1) {
        return;  // Se não houver processo corrente, não faz nada
    }

    // Recupera o processo corrente através do índice
    processo_t *proc_atual = &self->tabela_processos[self->index_processo_atual];

    // Salva os valores dos registradores do processo na memória
    mem_le(self->mem, IRQ_END_PC, &proc_atual->pc);
    mem_le(self->mem, IRQ_END_modo, (int*)&proc_atual->modo);
    mem_le(self->mem, IRQ_END_A, &proc_atual->a);
    mem_le(self->mem, IRQ_END_X, &proc_atual->x);

    console_printf("SO: Estado do processo %d salvo. PC: %d, A: %d, X: %d, Modo: %d\n", 
                   proc_atual->pid, proc_atual->pc, proc_atual->a, proc_atual->x, proc_atual->modo);
}

static void so_trata_pendencias(so_t *self)
{
  bool processos_ativos = false;

  // Verificar se ainda há processos prontos ou executando
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    processo_t *proc = &self->tabela_processos[i];
    
    // Se encontrar algum processo PRONTO ou EXECUTANDO, há processos ativos
    if (proc->estado == PRONTO || proc->estado == EXECUTANDO) {
      processos_ativos = true;
      break;
    }
  }

  // Se não há processos ativos, pode desbloquear os processos em espera
  if (!processos_ativos) {
    for (int i = 0; i < MAX_PROCESSOS; i++) {
      processo_t *proc = &self->tabela_processos[i];
      
      // Se o processo está bloqueado e o PID esperado foi atendido (bloqueio resolvido)
      if (proc->estado == BLOQUEADO) {
        proc->estado = PRONTO;  // Desbloqueia o processo e coloca como PRONTO
        console_printf("SO: Processo %d desbloqueado\n", proc->pid);
      }
    }
  }
}

static void so_escalona(so_t *self)
{
    console_printf("SO: Debug da tabela de processos:\n");
    for (int i = 0; i < MAX_PROCESSOS; i++) {
        processo_t *proc = &self->tabela_processos[i];
        console_printf("PID: %d, Estado: %d, PC: %d, A: %d, X: %d, Modo: %d PID Esperado %d\n", 
                       proc->pid, proc->estado, proc->pc, proc->a, proc->x, proc->modo, proc->pid_esperado);
    }

    // Verifica se o processo corrente não pode continuar (não existe ou está finalizado)
    if (self->index_processo_atual == -1 || self->tabela_processos[self->index_processo_atual].estado == FINALIZADO || self->tabela_processos[self->index_processo_atual].estado == BLOQUEADO) {
        // Se não há processo corrente ou o processo atual terminou, procuramos um novo processo pronto na tabela
        for (int i = 0; i < MAX_PROCESSOS; i++) {
            processo_t *proc = &self->tabela_processos[i];
            
            // Ignora processos BLOQUEADOS e FINALIZADOS
            if (proc->estado != BLOQUEADO && proc->estado != FINALIZADO) {
                // Se encontramos um processo pronto, definimos como o processo corrente
                if (proc->estado == PRONTO) {
                    self->index_processo_atual = i;  // Atualiza o índice do processo atual
                    proc->estado = EXECUTANDO;       // Marca o processo como em execução
                    console_printf("SO: Escalonamento: Processo %d iniciado.\n", proc->pid);
                    return;  // Processo escalonado com sucesso, retornamos
                }
            }
        }

        // Se nenhum processo pronto foi encontrado, significa que todos estão bloqueados ou finalizados
        console_printf("SO: Nenhum processo pronto para ser escalonado.\n");
    }
}

static int so_despacha(so_t *self)
{
    // Verifica se há erro interno, retornando 1 em caso afirmativo
    if (self->erro_interno) {
        return 1;
    }

    // Se não houver processo corrente (não há processo a ser despachado)
    if (self->index_processo_atual == -1) {
        return 1;  // Retorna 1, pois não há processo a ser despachado
    }

    // Recupera o processo atual da tabela de processos através do índice
    processo_t *proc_atual = &self->tabela_processos[self->index_processo_atual];

    // Salva o estado do processo corrente na memória
    // Os valores dos registradores são copiados para a memória
    mem_escreve(self->mem, IRQ_END_PC, proc_atual->pc);  // Salva o valor do PC
    mem_escreve(self->mem, IRQ_END_modo, proc_atual->modo);  // Salva o modo de operação
    mem_escreve(self->mem, IRQ_END_A, proc_atual->a);  // Salva o registrador A
    mem_escreve(self->mem, IRQ_END_X, proc_atual->x);  // Salva o registrador X

    console_printf("SO: Estado do processo %d salvo. PC: %d, A: %d, X: %d, Modo: %d\n", 
                   proc_atual->pid, proc_atual->pc, proc_atual->a, proc_atual->x, proc_atual->modo);

    return 0;  // Retorna 0 indicando que o processo foi salvo com sucesso
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

void so_inicializa_processo(so_t *self, int pid, int ender)
{
    // Percorre a tabela de processos para tentar encontrar um índice livre
    for (int i = 0; i < MAX_PROCESSOS; i++) {
        // Se encontrar um índice com PID_NENHUM (indicando um slot livre ou um processo finalizado)
        if (self->tabela_processos[i].pid == PID_NENHUM) {
            // Atribui o PID e inicializa o processo
            self->tabela_processos[i].pid = pid;    // Atribui o PID
            self->tabela_processos[i].pc = ender;   // Define o PC com o endereço de carga
            self->tabela_processos[i].a = 0;        // Zera o registrador A
            self->tabela_processos[i].x = 0;        // Zera o registrador X
            self->tabela_processos[i].estado = PRONTO; // Define o estado como PRONTO
            self->tabela_processos[i].modo = USUARIO;  // Define o modo como USUARIO

            // Atualiza o index do processo atual
            self->index_processo_atual = i;
            self->pid_processo_atual = pid;

            console_printf("SO: Processo %d criado com sucesso. Endereço de carga: %d\n", pid, ender);
            return;  // Processo alocado com sucesso
        }
    }

    // Caso não encontre índice livre
    console_printf("Erro: Tabela de processos cheia, não foi possível alocar o processo.\n");
}

static void so_trata_irq_reset(so_t *self)
{
  // 1. Carrega o programa 'init.maq' na memória
  int ender = so_carrega_programa(self, "init.maq");
  if (ender != 100) {
    console_printf("SO: problema na carga do programa inicial\n");
    self->erro_interno = true;
    return;
  }

  int pid_init = self->contador_pid;
  self->contador_pid++;

  // Inicializa o processo 'init' com o PID gerado
  so_inicializa_processo(self, pid_init, ender);

  console_printf("SO: Processo inicial 'init' criado com PID %d e endereço de carga %d\n", pid_init, ender);
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

// Implementação da chamada de sistema SO_CRIA_PROC
// Cria um processo novo e inicializa seus parâmetros
static void so_chamada_cria_proc(so_t *self)
{
    // 1. Recupera o endereço onde está o nome do arquivo (armazenado em X)
    int ender_nome_prog;
    if (mem_le(self->mem, IRQ_END_X, &ender_nome_prog) != ERR_OK) {
        mem_escreve(self->mem, IRQ_END_A, -1); // Erro ao ler endereço
        return;
    }

    // 2. Copia o nome do programa da memória
    char nome_prog[100];
    if (!copia_str_da_mem(100, nome_prog, self->mem, ender_nome_prog)) {
        mem_escreve(self->mem, IRQ_END_A, -1); // Erro ao copiar o nome do programa
        return;
    }

    // 3. Carrega o programa na memória
    int ender_carga = so_carrega_programa(self, nome_prog);
    if (ender_carga <= 0) {
        mem_escreve(self->mem, IRQ_END_A, -1); // Erro ao carregar o programa
        return;
    }

    // 4. Busca um índice livre na tabela de processos para o novo processo
    int pid_novo = self->contador_pid++; // Gera um novo PID
    int index_livre = -1;
    for (int i = 0; i < MAX_PROCESSOS; i++) {
        if (self->tabela_processos[i].pid == PID_NENHUM) {
            index_livre = i;
            break;
        }
    }
    if (index_livre == -1) { // Sem espaço na tabela de processos
        mem_escreve(self->mem, IRQ_END_A, -1);
        return;
    }

    // 5. Inicializa o processo na tabela
    processo_t *proc_novo = &self->tabela_processos[index_livre];
    proc_novo->pid = pid_novo;
    proc_novo->pc = ender_carga; // Define o PC para o endereço de carga
    proc_novo->a = 0;
    proc_novo->x = 0;
    proc_novo->estado = PRONTO; // Define como PRONTO para execução
    proc_novo->modo = USUARIO;

    // 6. Retorna o PID do novo processo no registrador A do processo chamador
    mem_escreve(self->mem, IRQ_END_A, pid_novo);
}

// implementação da chamada de sistema SO_MATA_PROC
static void so_chamada_mata_proc(so_t *self)
{
    int pid_matar;
    
    // Lê o PID que deve ser terminado (0 = o processo corrente)
    mem_le(self->mem, IRQ_END_X, &pid_matar);

    // Se for o processo corrente, mata ele
    if (pid_matar == 0 || pid_matar == self->tabela_processos[self->index_processo_atual].pid) {
        console_printf("SO: Processo %d finalizado.\n", self->tabela_processos[self->index_processo_atual].pid);
        
        // Marca o processo como finalizado
        self->tabela_processos[self->index_processo_atual].estado = FINALIZADO;

        // Escalona o próximo processo
        so_escalona(self);
    } else {
        console_printf("SO: Tentando matar processo inválido ou inexistente.\n");
        mem_escreve(self->mem, IRQ_END_A, -1);  // Indica erro
    }
}

// implementação da chamada se sistema SO_ESPERA_PROC
// espera o fim do processo com pid X
// implementação da chamada de sistema SO_ESPERA_PROC
// coloca o processo atual em espera e chama o próximo
static void so_chamada_espera_proc(so_t *self)
{
    // Coloca o processo atual em estado de bloqueio
    if (self->index_processo_atual != -1) {
        self->tabela_processos[self->index_processo_atual].estado = BLOQUEADO;
        console_printf("SO: Processo %d colocado em espera.\n", self->tabela_processos[self->index_processo_atual].pid);
    }
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

/*
 * tsh - Um pequeno shell para controle de tarefas
 *
 * Arthur Filipe Martins Nascimento - nUSP 5634455
 * Rafael Piovesan da Costa Machado - nUSP 5634945
 * Victor Zuim - nUSP5713860
 */


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <readline/readline.h>
#include <fcntl.h>
#include <signal.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

/* constantes gerais */
#define MAXLINE    1024		/* maximo tamanho da linha */
#define MAXARGS     128		/* maximo de argumentos em uma linha de comando */
#define MAXJOBS      16		/* maximo de tarefas executando ao mesmo tempo */
#define MAXJID    1<<16		/* maximo ID de tarefa */

/* estados das tarefas */
#define UNDEF 0				/* indefinido */
#define FG 1				/* executando em foreground */
#define BG 2				/* executando em background */
#define ST 3				/* parada */

#define modo (S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP)

/*
 * Estados das tarefas: FG (foreground), BG (background), ST (parada)
 * Transicoes de estados das tarefas e acoes responsaveis:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : comando fg
 *     ST -> BG  : comando bg
 *     BG -> FG  : comando fg
 * No maximo um programa pode estar em foreground.
 */

/* variaveis globais */
extern char **environ;				/* definido na libc */
char prompt[] = "tsh> ";			/* prompt de comando (NAO MUDE ISSO) */
int verbose = 0;					/* quanto de informacoes adicionais */
int nextjid = 1;					/* proxima JID para alocar */
char sbuf[MAXLINE];					/* para compor as mensagens sprintf */

struct job_t						/* A estrutura da tarefa */
{
	pid_t pid;						/* PID */
	int jid;						/* JID [1, 2, ...] */
	int state;						/* UNDEF, BG, FG, ou ST */
	char cmdline[MAXLINE];			/* linha d ecomando */
};
struct job_t jobs[MAXJOBS];			/* A lista de tarefas */
/* Fim das variaveis globais */

/* Prototipos de funcoes */

/* Aqui estao as funcoes que nos vamos implementar */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Aqui estao as funcoes auxiliares que nos fornecemos para voce */
int parseline(const char *cmdline, char **argv);
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs);
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid);
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid);
int pid2jid(pid_t pid);
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - A rotina principal do shell
 */
int main(int argc, char **argv)
{
	char c;
	char *cmdline;
	int emit_prompt = 1;			/* emitir o prompt (padrao) */

	/* Redireciona stderr para stdout (para que aquele driver receba
		toda saida do pipe conectado ao stdout) */
	dup2(1, 2);

	/* separa a lina de comando */
	while ((c = getopt(argc, argv, "hvp")) != EOF) {
		switch (c) {
			case 'h':				/* imprime a mensagem de ajuda */
				usage();
				break;
			case 'v':				/* imprime informacoes adicionais */
				verbose = 1;
				break;
			case 'p':				/* nao mostra o prompt */
				emit_prompt = 0;	/* util para testes automaticos */
				break;
			default:
				usage();
		}
	}

	/* Instala os handlers de sinal */

	/* Estes sao os que voce tera que implementar */
	Signal(SIGINT,  sigint_handler);	/* ctrl-c */
	Signal(SIGTSTP, sigtstp_handler);	/* ctrl-z */
	Signal(SIGCHLD, sigchld_handler);	/* filho terminou ou parou */

	/* Este fornece uma forma limpa de se matar o shell */
	Signal(SIGQUIT, sigquit_handler);

	/* Inicializa a lista de tarefas */
	initjobs(jobs);

	/* Executa o laco de leitura/execucao do shell */
	while (1) {

		/* Le a linha de comando */
		if (emit_prompt)
			cmdline = readline(prompt);
		else
			cmdline = readline(NULL);
		if (cmdline == NULL)		/* Fim de arquivo (ctrl-d) */
			exit(0);

		/* Avalia/executa a linha de comando */
		eval(cmdline);
		free(cmdline);
	}

	return 0;						/* O processo nunca chega aqui */
}


/*
 * eval - Avalia a linha de comando que o usuario acabou de escrever
 *
 * Se o usuario pediu um comando do shell (quit, jobs, bg ou fg) entao
 * executa ele imediatamente. Caso contrario, gera um processo-filho
 * e roda a tarefa no contexto do filho. Se a tarefa esta rodando em
 * primeiro plano, espera o seu termino e retorna. Nota: cada processo
 * filho deve ter um ID de grupo de processo diferente para que as tarefas
 * em segundo plano nao recebam SIGINT (ctrl-c) e SIGTSTP (ctrl-z)
 * do kernel.
 */
void eval(char *cmdline)
{
	char *argv[MAXARGS], *buf = cmdline, *end = cmdline + strlen(cmdline),
		*arq, comando[MAXLINE];
	int background = 0, argc, ES[3], p[2], i, vai_para_pipe = 0;
	pid_t pid, gid = 0;
	sigset_t mask, oldmask;

	strcpy(comando, cmdline);

	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);

	while ((buf<end) && isspace(*end))	/* ignora os espacos do final */
		end--;

	if (buf>=end)
		return;

	background = (*end=='&');

	for (i = 0; i < 3; i++)
		ES[i] = i;

	void parse (void) {

		argc = 0;
		argv[0] = NULL;

		vai_para_pipe = 0;

		do {
			while ((buf<end) && isspace(*buf))	/* ignora espacos do comeco */
				buf++;

			if (buf >= end)
				break;

			/* fim dos argumentos de um programa */
			if (*buf == '|') {
				buf++;
				vai_para_pipe = 1;
				break;
			}

			/* a entrada sera redirecionada do arquivo */
			if (*buf == '<') {
				buf++;

				while ((buf<end) && isspace(*buf))	/* ignora os espacos */
					buf++;

				if (*buf == '\"') {
					arq = ++buf;
					while ((buf<end) && (*buf != '\"'))	/* vai ate o " */
						buf++;
				} else {
					arq = buf;
					while ((buf<end) && !isspace(*buf))	/* vai ate o ' ' */
						buf++;
				}
				*buf++ = '\0';					/* termina o nome com um \0 */

				if (ES[0] != 0)
					close(ES[0]);
				ES[0] = open(arq, O_RDONLY);
				if (ES[0] == -1) {
					ES[0] = 0;
					fprintf(stderr,
						"Erro ao abrir o arquivo %s para leitura: %s\n",
						arq, strerror(errno));
				}
				continue;
			}

			/* a saida padrao sera redirecionada para o arquivo (append) */
			if ((*buf == '>') && (*(buf+1) == '>')) {
				buf+=2;

				while ((buf<end) && isspace(*buf))	/* ignora os espacos */
					buf++;

				if (*buf == '\"') {
					arq = ++buf;
					while ((buf<end) && (*buf != '\"'))	/* vai ate o " */
						buf++;
				} else {
					arq = buf;
					while ((buf<end) && !isspace(*buf))	/* vai ate o ' ' */
						buf++;
				}
				*buf++ = '\0';					/* termina o nome com um \0 */

				if (ES[1] != 1)
					close(ES[1]);
				ES[1] = open(arq, O_CREAT|O_WRONLY|O_APPEND, modo);
				if (ES[1] == -1) {
					ES[1] = 1;
					fprintf(stderr,
						"Erro ao abrir o arquivo %s para escrita: %s\n",
						arq, strerror(errno));
				}
				continue;
			}

			/* a saida padrao sera redirecionada para o arquivo */
			if ((*buf == '>') || ((*buf == '1') && (*(buf+1) == '>'))) {
				if (*buf == '>')
					buf++;
				else
					buf+=2;

				while ((buf<end) && isspace(*buf))	/* ignora os espacos */
					buf++;

				if (*buf == '\"') {
					arq = ++buf;
					while ((buf<end) && (*buf != '\"'))	/* vai ate o " */
						buf++;
				} else {
					arq = buf;
					while ((buf<end) && !isspace(*buf))	/* vai ate o ' ' */
						buf++;
				}
				*buf++ = '\0';					/* termina o nome com um \0 */

				if (ES[1] != 1)
					close(ES[1]);
				ES[1] = open(arq, O_CREAT|O_WRONLY|O_TRUNC, modo);
				if (ES[1] == -1) {
					ES[1] = 1;
					fprintf(stderr,
						"Erro ao abrir o arquivo %s para escrita: %s\n",
						arq, strerror(errno));
				}
				continue;
			}

			/* a saida de erros sera redirecionada para o arquivo */
			if ((*buf == '2') && (*(buf+1) == '>')) {
				buf+=2;

				while ((buf<end) && isspace(*buf))	/* ignora os espacos */
					buf++;

				if (*buf == '\"') {
					arq = ++buf;
					while ((buf<end) && (*buf != '\"'))	/* vai ate o " */
						buf++;
				} else {
					arq = buf;
					while ((buf<end) && !isspace(*buf))	/* vai ate o ' ' */
						buf++;
				}
				*buf++ = '\0';					/* termina o nome com um \0 */

				if (ES[2] != 2)
					close(ES[2]);
				ES[2] = open(arq, O_CREAT|O_WRONLY|O_TRUNC, modo);
				if (ES[2] == -1) {
					ES[2] = 2;
					fprintf(stderr,
						"Erro ao abrir o arquivo %s para escrita: %s\n",
						arq, strerror(errno));
				}
				continue;
			}

			/* ambas as saidas serao redirecionadas para o arquivo */
			if ((*buf == '&') && (*(buf+1) == '>')) {
				buf+=2;

				while ((buf<end) && isspace(*buf))	/* ignora os espacos */
					buf++;

				if (*buf == '\"') {
					arq = ++buf;
					while ((buf<end) && (*buf != '\"'))	/* vai ate o " */
						buf++;
				} else {
					arq = buf;
					while ((buf<end) && !isspace(*buf))	/* vai ate o ' ' */
						buf++;
				}
				*buf++ = '\0';					/* termina o nome com um \0 */

				if (ES[1] != 1)
					close(ES[1]);
				if (ES[2] != 2)
					close(ES[2]);
				ES[1] = ES[2] = open(arq, O_CREAT|O_WRONLY|O_TRUNC, modo);
				if (ES[1] == -1) {
					ES[1] = 1;
					ES[2] = 2;
					fprintf(stderr,
						"Erro ao abrir o arquivo %s para escrita: %s\n",
						arq, strerror(errno));
				}
				continue;
			}

			/* fim da linha de comando */
			if (*buf == '&') {
				buf = end;
				background = 1;
				break;
			}

			if (*buf == '\"') {					/* argumento comeca com " */
				argv[argc++] = ++buf;
				while ((buf<end) && (*buf != '\"'))	/* vai ate o " */
					buf++;
			} else {								/* novo argumento */
				argv[argc++] = buf;
				while ((buf<end) && !isspace(*buf))	/* vai ate o ' ' */
					buf++;
			}
			*buf++ = '\0';				/* termina o argumento com um \0 */

		} while (buf<end);

		argv[argc] = NULL;

	}

	parse();

	if (!argv[0])
		return;
	if (strlen(argv[0]) == 0)
		return;

	/* se for um comando do shell, */
	if (builtin_cmd((char **) argv))
		/* execute-o e retorne */
		return;

	sigprocmask(SIG_BLOCK, &mask, &oldmask);
	pid = fork();

	if (pid) {
		/* processo pai */
		addjob(jobs, pid, (background)?BG:FG, comando);

		for (i = 0; i<3; i++)
			if (ES[i] != i)
				close(ES[i]);

		sigprocmask(SIG_SETMASK, &oldmask, NULL);

	} else {
		/* processo filho */
		if ((gid = setpgid(0, 0)) == -1) {
			fprintf(stderr,
				"Erro ao criar um novo grupo de processo: %s\n",
				strerror(errno));
			exit(-1);
		}

		while (vai_para_pipe && (!pid)) {
			if (pipe(p) == -1) {
				fprintf(stderr,
					"Erro ao criar o pipe: %s\n",
					strerror(errno));
				exit(-1);
			}

			pid = fork();
			if (pid) {
				close(p[0]);
				ES[1] = p[1];
			} else {
				if (setpgid(0, gid) == -1) {
					fprintf(stderr,
						"Erro ao alterar o grupo de processo: %s\n",
						strerror(errno));
					exit(-1);
				}

				close(p[1]);
				ES[0] = p[0];
				ES[1] = 1;
				ES[2] = 2;

				parse();
			}
		}

		for (i = 0; i<3; i++)
			if (ES[i] != i) {
				dup2(ES[i], i);
				close(ES[i]);
			}

		sigprocmask(SIG_SETMASK, &oldmask, NULL);

		if (execve((char *) argv[0], (char **) &argv, environ)) {
			fprintf(stderr,
				"Erro ao executar %s: %s\n",
				argv[0], strerror(errno));
			exit(-1);
		}
	}

	if (!background)
		waitfg(pid);

	return;
}


/*
 * builtin_cmd - Se o usuario escreveu um comando do shell,
 * executa-o imediatamente.
 */
int builtin_cmd(char **argv)
{
	if (!strcmp(argv[0], "jobs")) {
		/* se for o comando jobs, lista as tarefas */
		listjobs(jobs);
		return 1;
	} else if (!strcmp(argv[0], "quit"))
		/* se for o comando quit, sai do shell */
		exit(0);
	else if ((!strcmp(argv[0], "fg")) || (!strcmp(argv[0], "bg"))) {
		/* se for fg ou bg, chama a funcao que faz isso */
		do_bgfg(argv);
		return 1;
	}

	return 0;								/* nao e um comando do shell*/
}


/*
 * do_bgfg - Executa os comandos bg e fg do shell
 */
void do_bgfg(char **argv)
{
	struct job_t * job=NULL;
	int ok, num;

	if (argv[1]==NULL) {
		fprintf(stderr, "Numero de processo nao informado\n");
		return;
	}

	if (argv[1][0]=='%') {
		/* le jids: "%jid" */
		if ((ok = sscanf(argv[1], "%%%d", &num)))
			job = getjobjid(jobs, num);
	} else {
		/* le pids: "pid" */
		if ((ok = sscanf(argv[1], "%d", &num)))
			job = getjobpid(jobs, num);
	}

	if ((!ok) || (job==NULL)) {
		fprintf(stderr, "Numero de processo nao reconhecido: %s\n", argv[1]);
		return;
	}

	kill(job->pid, SIGCONT);			/* continua o processo filho */
	if (!strcmp(argv[0], "fg")) {
		/* e se for foreground, espera ele terminar */
		job->state = FG;
		waitfg(job->pid);
	} else {
		job->state = BG;
	}

	return;
}


/*
 * waitfg - Bloqueia ate que o processo pid nao esteja mais em foreground
 */
void waitfg(pid_t pid)
{
	struct job_t * job;

	/* espera ocupada, como recomendado no shlab.pdf */
	while (1){
		job = getjobpid(jobs, pid);
		if (job == NULL)
			break;
		if (job->state != FG)
			break;
		if (usleep(500) != 0)
			break;
	}

	return;
}


/*****************
 * Funcoes de tratamento de sinais
 *****************/

/*
 * sigchld_handler - O kernel envia SIGCHLD ao shell quando um processo
 * filho termina ou para por receber um sinal SIGTSTP ou SIGSTOP. O
 * handler elimina todas as crianças zumbis, mas nao espera outras
 * criancas terminarem.
 */
void sigchld_handler(int sig)
{
	pid_t pid;
	int status;
	struct job_t * job;

	do {
		pid = waitpid(-1, &status, WNOHANG | WUNTRACED);
		if (pid > 0) {				/* um processo mudou de estado */
			job = getjobpid(jobs, pid);
			if (job == NULL)
				break;
			if (WIFEXITED(status)) {
				/* o processo retornou */
				if (job->state != FG)
					printf("O processo %%%d (pid %d) terminou com valor %d\n",
						job->jid, pid, WEXITSTATUS(status));
				deletejob(jobs, pid);
			} else if (WIFSIGNALED(status)) {
				/* o processo foi terminado com um sinal */
				if (job->state != FG)
					printf("O processo %%%d (pid %d) foi terminado com o sinal %d\n", job->jid, pid, WTERMSIG(status));
				deletejob(jobs, pid);
			} else if (WIFSTOPPED(status)) {
				/* o processo foi parado */
				job->state = ST;
			}
		} else						/* nenhum processo mudou de estado */
			break;
	} while (1);

	return;
}


/*
 * sigint_handler - O kernel envia o sinal SIGINT ao shell quando o usuario
 * digita ctrl-c no teclado. Capture ele e envie ao processo em primeiro
 * plano.
 */
void sigint_handler(int sig)
{
	pid_t pid;

	pid = fgpid(jobs);
	/* se houver um processo em foreground, */
	if (pid)
		/* repasse o sinal para ele */
		kill(-pid,sig);

	return;
}


/*
 * sigtstp_handler - O kernel envia o sinal SIGTSTP para o shell quando o
 * usuario digita ctrl-z no teclado. Capture ele e envie ao processo em
 * primeiro plano.
 */
void sigtstp_handler(int sig)
{
	pid_t pid;
	struct job_t * job;

	pid = fgpid(jobs);
	/* se houver um processo em foreground, */
	if (pid) {
		/* repasse o sinal para ele */
		kill(-pid,sig);
		/* e atualize as informacoes que temos dele */
		job = getjobpid(jobs, pid);
		job->state = ST;
	}

	return;
}


/*********************
 * Fim dos handlers de sinal
 *********************/

/***********************************************
 * Rotinas auxiliares de manipulacao da lista de tarefas
 **********************************************/

/* clearjob - Limpa as informacoes na estrutura da tarefa */
void clearjob(struct job_t *job)
{
	job->pid = 0;
	job->jid = 0;
	job->state = UNDEF;
	job->cmdline[0] = '\0';
}


/* initjobs - Inicializa a lista de tarefas */
void initjobs(struct job_t *jobs)
{
	int i;

	for (i = 0; i < MAXJOBS; i++)
		clearjob(&jobs[i]);
}


/* maxjid - Retorna o maior JID alocado */
int maxjid(struct job_t *jobs)
{
	int i, max=0;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid > max)
			max = jobs[i].jid;
	return max;
}


/* addjob - Coloca uma tarefa na lista de tarefas */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline)
{
	int i;

	if (pid < 1)
		return 0;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == 0) {
			jobs[i].pid = pid;
			jobs[i].state = state;
			jobs[i].jid = nextjid++;
			if (nextjid > MAXJOBS)
				nextjid = 1;
			strcpy(jobs[i].cmdline, cmdline);
			if(verbose) {
				printf("Added job [%d] %d %s\n", jobs[i].jid,
					jobs[i].pid, jobs[i].cmdline);
			}
			return 1;
		}
	}
	printf("Tried to create too many jobs\n");
	return 0;
}


/* deletejob - Deleta uma tarefa da lista de tarefas cujo PID=pid */
int deletejob(struct job_t *jobs, pid_t pid)
{
	int i;

	if (pid < 1)
		return 0;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == pid) {
			clearjob(&jobs[i]);
			nextjid = maxjid(jobs)+1;
			return 1;
		}
	}
	return 0;
}


/* fgpid - Retorna o PID da tarefa em foreground, ou 0 se nao houver uma */
pid_t fgpid(struct job_t *jobs)
{
	int i;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].state == FG)
			return jobs[i].pid;
	return 0;
}


/* getjobpid  - Encontra o PID na lista de tarefas */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid)
{
	int i;

	if (pid < 1)
		return NULL;
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid)
			return &jobs[i];
	return NULL;
}


/* getjobjid  - Encontra o JID na lista de tarefas */
struct job_t *getjobjid(struct job_t *jobs, int jid)
{
	int i;

	if (jid < 1)
		return NULL;
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid == jid)
			return &jobs[i];
	return NULL;
}


/* pid2jid - Mapeia o PID ao JID */
int pid2jid(pid_t pid)
{
	int i;

	if (pid < 1)
		return 0;
	for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid) {
		return jobs[i].jid;
	}
	return 0;
}


/* listjobs - Imprime as tarefas atuais */
void listjobs(struct job_t *jobs)
{
	int i;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid != 0) {
			printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
			switch (jobs[i].state) {
				case BG:
					printf("Running ");
					break;
				case FG:
					printf("Foreground ");
					break;
				case ST:
					printf("Stopped ");
					break;
				default:
					printf("listjobs: Internal error: job[%d].state=%d ",
						i, jobs[i].state);
			}
			printf("%s\n", jobs[i].cmdline);
		}
	}
}


/******************************
 * fim das funcoes de manipulacao da lista de tarefas
 ******************************/

/***********************
 * Outras funcoes auxiliares
 ***********************/

/*
 * uso - imprime uma mensadem de ajuda
 */
void usage(void)
{
	printf("Usage: shell [-hvp]\n");
	printf("   -h   print this message\n");
	printf("   -v   print additional diagnostic information\n");
	printf("   -p   do not emit a command prompt\n");
	exit(1);
}


/*
 * unix_error - rotina de tratamento de erros estilo unix
 */
void unix_error(char *msg)
{
	fprintf(stderr, "%s: %s\n", msg, strerror(errno));
	exit(1);
}


/*
 * app_error - rotina de tratamento de erros estilo aplicacao
 */
void app_error(char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(1);
}


/*
 * Signal - funcao envelope para a funcao sigaction
 */
handler_t *Signal(int signum, handler_t *handler)
{
	struct sigaction action, old_action;

	action.sa_handler = handler;
	sigemptyset(&action.sa_mask);	/* bloqueia os sinais tratados */
	action.sa_flags = SA_RESTART;	/* reinicia as syscall se possivel */

	if (sigaction(signum, &action, &old_action) < 0)
		unix_error("Signal error");
	return (old_action.sa_handler);
}


/*
 * sigquit_handler - O programa pai pode terminar o filho enviando o
 * sinal SIGQUIT.
 */
void sigquit_handler(int sig)
{
	printf("Terminating after receipt of SIGQUIT signal\n");
	exit(1);
}

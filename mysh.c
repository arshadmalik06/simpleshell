#define _POSIX_C_SOURCE 200809L   /* gives us getline(), sigaction(), strdup() */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Limits and data structures                                                */
#define MAX_TOKENS 256 
#define MAX_ARGS    64   
#define MAX_CMDS    16  

/* A token is either a "word" (command name / argument / filename)
 * or an "operator" (one of  |  <  >  >>  & ).                               */
typedef struct {
    char *text;
    int   is_op;
} Token;

/* One command, e.g.  grep foo < in.txt > out.txt                            */
typedef struct {
    char *argv[MAX_ARGS];  /* NULL-terminated argument list for execvp()    */
    char *infile;          /* file after '<'  (NULL if none)                */
    char *outfile;         /* file after '>' or '>>' (NULL if none)         */
    int   append;          /* 1 if '>>' was used, 0 for '>'                 */
} Command;

/* A whole input line: one or more commands joined by pipes.                 */
typedef struct {
    Command cmds[MAX_CMDS];
    int     ncmds;
    int     background;    /* 1 if the line ended with '&'                  */
} Pipeline;

/* Shell-wide state */
static int running   = 1;  /* main loop runs while this is 1                */
static int exit_code = 0;  /* value returned by main() / the 'exit' builtin */

/* STAGE 4: builtins                                                         */

static void builtin_cd(char **argv)
{
    const char *target = argv[1];

    if (target == NULL) {              /* plain "cd" goes to $HOME           */
        target = getenv("HOME");
        if (target == NULL) {
            fprintf(stderr, "mysh: cd: HOME not set\n");
            return;
        }
    }
    if (chdir(target) != 0)
        fprintf(stderr, "mysh: cd: %s: %s\n", target, strerror(errno));
}

static void builtin_pwd(char **argv)
{
    char buf[4096];
    (void)argv;
    if (getcwd(buf, sizeof buf) != NULL)
        printf("%s\n", buf);
    else
        perror("mysh: pwd");
}

static void builtin_help(char **argv)
{
    (void)argv;
    printf("mysh - a simple shell\n");
    printf("Builtins:\n");
    printf("  cd [dir]     change directory (no argument = $HOME)\n");
    printf("  pwd          print current directory\n");
    printf("  help         show this message\n");
    printf("  exit [n]     leave the shell with status n (default 0)\n");
    printf("Features:\n");
    printf("  cmd arg ...        run a program\n");
    printf("  cmd < in > out     redirect input / output  (>> appends)\n");
    printf("  a | b | c          pipelines\n");
    printf("  cmd &              run in the background\n");
    printf("  'quoted words'     single or double quotes keep spaces together\n");
}

static void builtin_exit(char **argv)
{
    if (argv[1] != NULL)
        exit_code = atoi(argv[1]);
    running = 0;                       /* main loop sees this and stops      */
}

/* A table mapping names to functions: to add a builtin, add one line here.
 * (Table-driven dispatch, as described in Brennan's builtins section.)      */
typedef struct {
    const char *name;
    void (*func)(char **argv);
} Builtin;

static const Builtin builtins[] = {
    { "cd",   builtin_cd   },
    { "pwd",  builtin_pwd  },
    { "help", builtin_help },
    { "exit", builtin_exit },
};
#define NUM_BUILTINS (sizeof builtins / sizeof builtins[0])

/* Return the builtin called `name`, or NULL if it is not a builtin. */
static const Builtin *find_builtin(const char *name)
{
    for (size_t i = 0; i < NUM_BUILTINS; i++)
        if (strcmp(name, builtins[i].name) == 0)
            return &builtins[i];
    return NULL;
}
/* STAGE 2: tokenizer and parser                                             */
/*                                                                           */
/* Brennan's tutorial splits on whitespace with strtok(). We do a small      */
/* hand-written scanner instead because we also need:                        */
/*   - operators that may touch words:   ls>out   a|b                        */
/*   - quotes:                           echo "hello   world"                */

static void free_tokens(Token tokens[], int n)
{
    for (int i = 0; i < n; i++)
        free(tokens[i].text);
}

/* Turn `line` into tokens. Returns the token count, or -1 on error. */
static int tokenize(const char *line, Token tokens[])
{
    int n = 0;
    const char *p = line;
    /* A word can never be longer than the whole line, so this is enough. */
    char *word = malloc(strlen(line) + 1);
    if (word == NULL) { perror("mysh: malloc"); return -1; }

    while (*p) {
        if (isspace((unsigned char)*p)) { p++; continue; }

        if (n >= MAX_TOKENS - 1) {
            fprintf(stderr, "mysh: too many tokens\n");
            goto fail;
        }

        /* --- operator token: |  <  >  >>  & --- */
        if (strchr("|<>&", *p)) {
            if (p[0] == '>' && p[1] == '>') {
                tokens[n].text = strdup(">>");
                p += 2;
            } else {
                char op[2] = { *p, '\0' };
                tokens[n].text = strdup(op);
                p++;
            }
            tokens[n].is_op = 1;
            n++;
            continue;
        }

        /* --- word token: runs until unquoted whitespace or an operator --- */
        size_t len = 0;
        char quote = 0;                 /* 0 = not in quotes, else ' or "    */
        while (*p) {
            if (quote) {
                if (*p == quote) { quote = 0; p++; continue; }  /* closing  */
            } else {
                if (*p == '\'' || *p == '"') { quote = *p; p++; continue; }
                if (isspace((unsigned char)*p) || strchr("|<>&", *p))
                    break;              /* word ends here                    */
            }
            word[len++] = *p++;
        }
        if (quote) {
            fprintf(stderr, "mysh: unmatched quote\n");
            goto fail;
        }
        word[len] = '\0';
        tokens[n].text  = strdup(word);
        tokens[n].is_op = 0;
        n++;
    }

    free(word);
    return n;

fail:
    free(word);
    free_tokens(tokens, n);
    return -1;
}

/* Turn tokens into a Pipeline.
 * Returns 1 = ok, 0 = empty line, -1 = syntax error.
 * NOTE: argv[] entries point INTO the token strings, so the tokens must stay
 * alive until the command has finished running.                             */
static int parse(Token tokens[], int n, Pipeline *pl)
{
    memset(pl, 0, sizeof *pl);          /* also NULL-terminates every argv   */
    if (n == 0) return 0;

    pl->ncmds = 1;
    Command *cur = &pl->cmds[0];
    int argc = 0;

    for (int i = 0; i < n; i++) {
        const Token *t = &tokens[i];

        if (!t->is_op) {                /* ordinary word -> next argument    */
            if (argc >= MAX_ARGS - 1) {
                fprintf(stderr, "mysh: too many arguments\n");
                return -1;
            }
            cur->argv[argc++] = t->text;
        }
        else if (strcmp(t->text, "|") == 0) {
            if (argc == 0) {
                fprintf(stderr, "mysh: syntax error near '|'\n");
                return -1;
            }
            if (pl->ncmds >= MAX_CMDS) {
                fprintf(stderr, "mysh: pipeline too long\n");
                return -1;
            }
            cur = &pl->cmds[pl->ncmds++];   /* start the next command        */
            argc = 0;
        }
        else if (strcmp(t->text, "<") == 0 ||
                 strcmp(t->text, ">") == 0 ||
                 strcmp(t->text, ">>") == 0) {
            if (i + 1 >= n || tokens[i + 1].is_op) {
                fprintf(stderr, "mysh: expected filename after '%s'\n", t->text);
                return -1;
            }
            char *file = tokens[++i].text;  /* consume the filename token    */
            if (t->text[0] == '<') {
                cur->infile = file;
            } else {
                cur->outfile = file;
                cur->append  = (t->text[1] == '>');
            }
        }
        else {                          /* '&' : only allowed at the very end */
            if (i != n - 1) {
                fprintf(stderr, "mysh: '&' must be at the end of the line\n");
                return -1;
            }
            pl->background = 1;
        }
    }

    if (argc == 0) {                    /* e.g. "ls |"  or just "&"          */
        fprintf(stderr, "mysh: syntax error: missing command\n");
        return -1;
    }
    return 1;
}

/* ========================================================================= */
/* STAGE 3 + 5 + 6: running commands                                         */
/* ========================================================================= */

/* The code that runs inside a forked CHILD process.
 *   i       = position of this command in the pipeline (0 = first)
 *   pipes   = all pipe fd pairs; pipes[k] connects command k -> command k+1
 * This function never returns: it either exec()s or _exit()s.               */
static void run_child(const Command *c, int i, int ncmds, int pipes[][2],
                      int background)
{
    /* STAGE 6: the shell catches Ctrl-C; the child must go back to the
     * default action (die) -- except background jobs, which ignore it.      */
    signal(SIGINT, background ? SIG_IGN : SIG_DFL);

    /* STAGE 5 (pipes): dup2(a, b) makes fd b a copy of fd a.
     * Command i reads from the previous pipe and writes into the next one.  */
    if (i > 0)
        dup2(pipes[i - 1][0], STDIN_FILENO);
    if (i < ncmds - 1)
        dup2(pipes[i][1], STDOUT_FILENO);

    /* Now that 0/1 are wired up, close EVERY pipe fd in this child. If we
     * left a write end open, the reader would never see end-of-file.        */
    for (int k = 0; k < ncmds - 1; k++) {
        close(pipes[k][0]);
        close(pipes[k][1]);
    }

    /* A background job must not fight the shell for the keyboard. */
    if (background && i == 0 && c->infile == NULL) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
    }

    /* STAGE 5 (redirection): files override pipes, like in a real shell.    */
    if (c->infile) {
        int fd = open(c->infile, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "mysh: %s: %s\n", c->infile, strerror(errno));
            _exit(1);
        }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (c->outfile) {
        int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
        int fd = open(c->outfile, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "mysh: %s: %s\n", c->outfile, strerror(errno));
            _exit(1);
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }

    /* A builtin used inside a pipeline (e.g. "pwd | cat") runs in the child. */
    const Builtin *b = find_builtin(c->argv[0]);
    if (b) {
        b->func((char **)c->argv);
        fflush(stdout);
        _exit(0);
    }

    /* STAGE 3: replace this child with the requested program.
     * execvp = exec + 'v' (argument vector) + 'p' (search $PATH).
     * If it returns, something went wrong.                                  */
    execvp(c->argv[0], c->argv);
    fprintf(stderr, "mysh: %s: %s\n", c->argv[0], strerror(errno));
    _exit(127);                         /* 127 = "command not found" by convention */
}

/* Run a parsed line. */
static void execute(Pipeline *pl)
{
    /* STAGE 4: a lone builtin runs inside the shell process itself. */
    if (pl->ncmds == 1) {
        const Builtin *b = find_builtin(pl->cmds[0].argv[0]);
        if (b) {
            b->func(pl->cmds[0].argv);
            fflush(stdout);   /* keep output in order with child programs   */
            return;
        }
    }

    /* Flush before forking: otherwise unflushed text in our buffer would be
     * copied into every child and printed twice / out of order.            */
    fflush(stdout);

    /* STAGE 5: create one pipe between each pair of neighbouring commands.  */
    int npipes = pl->ncmds - 1;
    int pipes[MAX_CMDS][2];
    for (int k = 0; k < npipes; k++) {
        if (pipe(pipes[k]) < 0) {
            perror("mysh: pipe");
            for (int j = 0; j < k; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            return;
        }
    }

    /* STAGE 3: fork one child per command. */
    pid_t pids[MAX_CMDS];
    int started = 0;
    for (int i = 0; i < pl->ncmds; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("mysh: fork");
            break;
        }
        if (pid == 0)
            run_child(&pl->cmds[i], i, pl->ncmds, pipes, pl->background);
        pids[started++] = pid;          /* parent remembers each child's pid */
    }

    /* The parent does not use the pipes. Close them, otherwise readers
     * further down the pipeline would wait forever.                         */
    for (int k = 0; k < npipes; k++) {
        close(pipes[k][0]);
        close(pipes[k][1]);
    }

    /* STAGE 6: background job -> don't wait, just announce it. */
    if (pl->background) {
        if (started > 0 && isatty(STDIN_FILENO))
            printf("[background] pid %d\n", (int)pids[started - 1]);
        return;
    }

    /* STAGE 3: foreground job -> wait for every child to finish
     * (Gupta: waitpid is better than sleep() for ordering).                 */
    for (int i = 0; i < started; i++) {
        int status;
        while (waitpid(pids[i], &status, 0) < 0 && errno == EINTR)
            ;                           /* interrupted by Ctrl-C: wait again */
        if (WIFSIGNALED(status) && WTERMSIG(status) != SIGINT)
            fprintf(stderr, "mysh: process %d killed by signal %d (%s)\n",
                    (int)pids[i], WTERMSIG(status), strsignal(WTERMSIG(status)));
    }
}

/* ========================================================================= */
/* STAGE 6: signals and background-job cleanup                               */
/* ========================================================================= */

/* Ctrl-C sends SIGINT to the whole foreground group: the shell AND the child.
 * We don't want the shell to die, so it just prints a newline and carries on. */
static void on_sigint(int sig)
{
    (void)sig;
    (void)!write(STDOUT_FILENO, "\n", 1);   /* write() is signal-safe, printf is not */
}

/* Collect finished background children so they don't linger as zombies.
 * WNOHANG = "return immediately if nobody has finished".                    */
static void reap_background(void)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        if (isatty(STDIN_FILENO))
            printf("[done] pid %d\n", (int)pid);
}

/* ========================================================================= */
/* STAGE 1: main loop  (Read -> Parse -> Execute)                            */
/* ========================================================================= */

int main(void)
{
    /* Install the Ctrl-C handler. sa_flags = 0 (no SA_RESTART) so that a
     * blocked getline() returns early when Ctrl-C is pressed at the prompt. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    char   *line = NULL;                /* getline() allocates/grows this    */
    size_t  cap  = 0;
    int interactive = isatty(STDIN_FILENO);  /* hide the prompt when piped in */

    while (running) {
        reap_background();

        if (interactive) {
            printf("mysh> ");
            fflush(stdout);
        }

        /* READ */
        if (getline(&line, &cap, stdin) < 0) {
            if (feof(stdin)) {          /* Ctrl-D or end of script file      */
                if (interactive) putchar('\n');
                break;
            }
            clearerr(stdin);            /* interrupted by Ctrl-C: new prompt */
            continue;
        }

        /* PARSE */
        Token tokens[MAX_TOKENS];
        int n = tokenize(line, tokens);
        if (n < 0) continue;            /* error already printed             */

        Pipeline pl;
        int rc = parse(tokens, n, &pl);

        /* EXECUTE */
        if (rc > 0)
            execute(&pl);

        free_tokens(tokens, n);         /* argv pointed into these strings   */
    }

    free(line);
    return exit_code;
}

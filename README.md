# mysh: a simple Unix shell in C

OS course project. A small shell built stage by stage, following the order the reference tutorials teach it.

## Build and run

```
make
./mysh          # interactive
./test.sh       # runs a batch of sample commands
```

Needs Linux/macOS, gcc, make. Not Windows-native (use WSL).

## What it supports

| Feature | Example |
|---|---|
| Run any program (searches `$PATH`) | `ls -l /tmp` |
| Builtins | `cd`, `pwd`, `help`, `exit [n]` |
| Quotes | `echo "a   b"` |
| Redirection | `sort < in.txt > out.txt`, `echo hi >> log.txt` |
| Pipelines (any length) | `cat f \| grep x \| wc -l` |
| Background jobs | `sleep 5 &` |
| Ctrl-C | stops the running program, not the shell |
| Ctrl-D | exits the shell |

Not supported (by design, to keep it simple): `$VARIABLES`, wildcards (`*`), `2>&1`, job control (`fg`/`bg`), command history.

## How the code is organised (read `mysh.c` top to bottom)

The file is split into stages. Each stage maps to the reference that explains the idea.

| Stage in `mysh.c` | What it does | Where to read about it |
|---|---|---|
| 1. Main loop | Prompt, read a line, parse, execute, repeat | Brennan: "Basic loop of a shell"; Kamal workshop step 1; Danish Prakash: "Starting of the shell" |
| 2. Tokenizer + parser | Splits text into words and operators (`\| < > >> &`), builds a `Pipeline` of `Command`s | Brennan: "Parsing the line"; Danish Prakash: "Tokenizing Input" |
| 3. fork / exec / wait | Child process runs the program, parent waits | Brennan: "How shells start processes"; Gupta Part I (fork and waitpid); Kamal steps 2-5; tokenrove stage 1 |
| 4. Builtins | `cd`, `pwd`, `help`, `exit` run inside the shell itself | Brennan: "Shell Builtins"; Kamal step 5 (why `cd` can't be a child process) |
| 5. Redirection + pipes | `open()` + `dup2()` for files; `pipe()` + `dup2()` between commands | Kamal "Pipelines" and "IO redirection"; tokenrove stage 2 |
| 6. Signals + background | SIGINT handler, `&`, reaping finished background jobs | Kamal "Signal handling" / "Running in the background"; tokenrove stage 3 |

The code is written from scratch; the tutorials explain the ideas. Brennan's page itself asks students not to copy his code, so keep the references in your report and describe the design in your own words.

## The key ideas (good for your viva)

1. **A shell is just a loop**: read, parse, execute.
2. **fork() + exec()**: `fork` clones the shell; the clone calls `execvp` to turn itself into the requested program. The shell survives because only the clone was replaced.
3. **wait()**: the parent blocks until the child finishes, so the prompt doesn't print early.
4. **Why builtins exist**: the working directory belongs to a process. A `cd` run as a child would change the child's directory and exit. So the shell must call `chdir()` itself.
5. **File descriptors**: 0 = stdin, 1 = stdout, 2 = stderr. `dup2(a, b)` makes `b` point at the same thing as `a`. Redirection and pipes are only `dup2` calls done in the child before `exec`.
6. **Pipes**: `pipe()` returns two fds; what is written to one end is read from the other. Every process must close the pipe ends it doesn't use, otherwise the reader never sees end-of-file.
7. **Signals**: Ctrl-C goes to the shell and the child. The shell ignores its effect (just prints a newline); the child gets the default action (die).
8. **Zombies**: a finished child stays in the process table until its parent waits. `reap_background()` uses `waitpid(..., WNOHANG)` to clean up finished background jobs.

## Suggested way to study it later

Read `mysh.c` in this order: `main()` (stage 1) -> `tokenize()` and `parse()` (stage 2) -> `execute()` (stage 3) -> `run_child()` (stages 3, 5) -> builtins (stage 4). Then open the matching reference section from the table above.

## References

- Stephen Brennan, Write a Shell in C: https://brennan.io/2015/01/16/write-a-shell-in-c/
- Kamal Marhubi, shell-workshop: https://github.com/kamalmarhubi/shell-workshop
- Indradhanush Gupta, Writing a Unix Shell Part 1: https://igupta.in/blog/writing-a-unix-shell-part-1/
- tokenrove, build-your-own-shell: https://github.com/tokenrove/build-your-own-shell
- Danish Prakash, Write a shell in C: https://danishpraka.sh/posts/write-a-shell/

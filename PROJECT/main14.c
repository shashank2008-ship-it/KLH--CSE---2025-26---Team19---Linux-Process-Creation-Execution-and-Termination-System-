#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#define MAX_LINE 512
#define MAX_ARGS 64

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int counter = 0;

static void *count_worker(void *unused) {
    (void)unused;
    for (int i = 0; i < 10000; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

/* CO4: malloc/free and CO6: POSIX threads with mutex. Run once at startup. */
static void startup_checks(void) {
    int *number = malloc(sizeof(*number));
    if (number == NULL) { perror("malloc"); return; }
    *number = 42;
    printf("[CO4] Heap memory allocated: %d\n", *number);
    free(number);
    pthread_t a, b;
    int made_a = pthread_create(&a, NULL, count_worker, NULL);
    int made_b = pthread_create(&b, NULL, count_worker, NULL);
    if (made_a == 0) pthread_join(a, NULL);
    if (made_b == 0) pthread_join(b, NULL);
    if (made_a != 0 || made_b != 0) fprintf(stderr, "Thread creation failed\n");
    else printf("[CO6] Mutex-protected thread count: %d (expected 20000)\n", counter);
}

/* Split on whitespace: deliberately no quoting, globbing, or shell expansions. */
static int parse(char *text, char *argv[]) {
    int count = 0;
    char *word = strtok(text, " \t\r\n");
    while (word != NULL && count < MAX_ARGS - 1) {
        argv[count++] = word;
        word = strtok(NULL, " \t\r\n");
    }
    argv[count] = NULL;
    return count;
}

/* CO5: redirect output through open/dup2/close. */
static int redirect_output(char *argv[]) {
    for (int i = 0; argv[i] != NULL; i++) {
        if (strcmp(argv[i], ">") == 0) {
            if (argv[i + 1] == NULL || argv[i + 2] != NULL) {
                fprintf(stderr, "Usage: command > filename\n");
                return -1;
            }
            int fd = open(argv[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { perror("open"); return -1; }
            if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2"); close(fd); return -1; }
            close(fd);
            argv[i] = NULL;
            return 0;
        }
    }
    return 0;
}

/* CO2: fork, execvp, waitpid. */
static void run_single(char *argv[]) {
    pid_t child = fork();
    if (child < 0) { perror("fork"); return; }
    if (child == 0) {
        if (redirect_output(argv) < 0) _exit(1);
        execvp(argv[0], argv);
        perror("execvp");
        _exit(127);
    }
    printf("[CO2] Parent PID=%ld, Child PID=%ld\n", (long)getpid(), (long)child);
    fflush(stdout);
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("waitpid"); return;
    }
    if (WIFEXITED(status)) printf("[CO2] Child PID %ld exited with code %d\n", (long)child, WEXITSTATUS(status));
    else if (WIFSIGNALED(status)) printf("[CO2] Child PID %ld ended by signal %d\n", (long)child, WTERMSIG(status));
}

/* CO2: Show one temporary zombie using the standard Ubuntu ps command.
   The parent reaps the child immediately after the demonstration. */
static void zombie_demo(void) {
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) { perror("fork zombie"); return; }
    if (child == 0) _exit(0);
    /* wait until the child has exited, without reaping it */
    for (int i = 0; i < 100; i++) {
        char path[64], state = '?';
        snprintf(path, sizeof(path), "/proc/%ld/stat", (long)child);
        FILE *fp = fopen(path, "r");
        if (fp != NULL) {
            int pid; char comm[256];
            if (fscanf(fp, "%d %255s %c", &pid, comm, &state) != 3) state = '?';
            fclose(fp);
        }
        if (state == 'Z') break;
        struct timespec delay = {0, 10000000L};
        nanosleep(&delay, NULL);
    }
    printf("\n[CO2] Zombie process demonstration (Z means zombie):\n");
    fflush(stdout);
    pid_t ps_child = fork();
    if (ps_child == 0) {
        char pid_text[32];
        snprintf(pid_text, sizeof(pid_text), "%ld", (long)child);
        execlp("ps", "ps", "-o", "pid,ppid,stat,comm", "-p", pid_text, (char *)NULL);
        perror("ps"); _exit(127);
    }
    if (ps_child > 0) waitpid(ps_child, NULL, 0);
    else perror("fork ps");
    int status;
    if (waitpid(child, &status, 0) >= 0)
        printf("[CO2] Parent cleaned up zombie PID %ld using waitpid()\n\n", (long)child);
    else perror("waitpid zombie");
}

/* CO3: one anonymous pipe between two Linux programs. */
static void run_pipe(char *left[], char *right[]) {
    int p[2];
    if (pipe(p) < 0) { perror("pipe"); return; }
    pid_t first = fork();
    if (first < 0) { perror("fork"); close(p[0]); close(p[1]); return; }
    if (first == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0) _exit(1);
        close(p[1]);
        execvp(left[0], left);
        perror("execvp left"); _exit(127);
    }
    pid_t second = fork();
    if (second < 0) {
        perror("fork"); close(p[0]); close(p[1]);
        waitpid(first, NULL, 0); return;
    }
    if (second == 0) {
        close(p[1]);
        if (dup2(p[0], STDIN_FILENO) < 0) _exit(1);
        close(p[0]);
        if (redirect_output(right) < 0) _exit(1);
        execvp(right[0], right);
        perror("execvp right"); _exit(127);
    }
    close(p[0]); close(p[1]);
    waitpid(first, NULL, 0);
    waitpid(second, NULL, 0);
    printf("[CO3] Pipe completed between child PIDs %ld and %ld\n", (long)first, (long)second);
}

int main(int argc, char *argv[]) {
    char line[MAX_LINE];
    printf("Linux Process Creation, Execution and Termination System\n");
    printf("Type Linux commands: ls, pwd, ps, date, echo, cat, wc, etc.\n");
    printf("Supports one pipe (|), output redirect (>), cd and exit.\n");
    /* Keep the normal shell output clean. Run demos only with --demo. */
    if (argc > 1 && strcmp(argv[1], "--demo") == 0) {
        startup_checks();
        zombie_demo();
    }
    while (1) {
        printf("os> "); fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) { putchar('\n'); break; }
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '\0') continue;
        char *pipe_at = strchr(line, '|');
        char *left[MAX_ARGS], *right[MAX_ARGS];
        if (pipe_at != NULL) {
            *pipe_at = '\0';
            if (strchr(pipe_at + 1, '|') != NULL) {
                fprintf(stderr, "Only one pipe is supported\n"); continue;
            }
            if (parse(line, left) == 0 || parse(pipe_at + 1, right) == 0) {
                fprintf(stderr, "Usage: command | command\n"); continue;
            }
            run_pipe(left, right);
            continue;
        }
        if (parse(line, left) == 0) continue;
        if (strcmp(left[0], "exit") == 0) break;
        if (strcmp(left[0], "cd") == 0) {
            const char *target = left[1] ? left[1] : getenv("HOME");
            if (target == NULL || chdir(target) != 0) perror("cd");
            continue;
        }
        run_single(left);
    }
    pthread_mutex_destroy(&lock);
    puts("Goodbye!");
    return 0;
}

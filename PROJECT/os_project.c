#define _GNU_SOURCE

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

#define MAX_CMD 512
#define MAX_ARGS 64
#define LOG_FILE "process_lifecycle.log"

/* CO6: shared report state protected by a mutex */
static pthread_mutex_t report_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int child_running = 0;
static pid_t monitored_child = -1;

/* CO1: use the write() system call to display a message */
void os_write(const char *message)
{
    write(STDOUT_FILENO, message, strlen(message));
}

/* CO4: allocate command memory dynamically */
char *allocate_command(const char *input)
{
    size_t size = strlen(input) + 1;
    char *memory = malloc(size);

    if (memory == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    strcpy(memory, input);
    return memory;
}

/* Turn "ls -l" into argv for execvp(). */
int split_command(char *command, char *args[])
{
    int count = 0;
    char *token = strtok(command, " \t\n");

    while (token != NULL && count < MAX_ARGS - 1) {
        args[count++] = token;
        token = strtok(NULL, " \t\n");
    }

    args[count] = NULL;
    return count;
}

/* CO5: append lifecycle information to a file. */
void log_message(const char *message)
{
    int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);

    if (fd == -1) {
        perror("open log file");
        return;
    }

    write(fd, message, strlen(message));
    close(fd);
}

/* CO6: a separate POSIX thread monitors the parent-side child state. */
void *monitor_thread(void *arg)
{
    (void)arg;

    while (1) {
        pthread_mutex_lock(&report_mutex);
        int running = child_running;
        pid_t pid = monitored_child;
        pthread_mutex_unlock(&report_mutex);

        if (!running)
            break;

        printf("[CO6] Monitor thread: child PID %d is still running...\n", pid);
        sleep(1);
    }

    printf("[CO6] Monitor thread: child has finished. Mutex synchronization complete.\n");
    return NULL;
}

void print_separator(void)
{
    printf("------------------------------------------------------------\n");
}

void print_status(int status)
{
    if (WIFEXITED(status)) {
        printf("Child exit status : %d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        printf("Child terminated by signal : %d\n", WTERMSIG(status));
    } else if (WIFSTOPPED(status)) {
        printf("Child stopped by signal : %d\n", WSTOPSIG(status));
    }
}

void run_lifecycle(const char *input)
{
    char *command_memory = allocate_command(input); /* CO4 */
    char *args[MAX_ARGS];

    if (split_command(command_memory, args) == 0) {
        printf("No command entered.\n");
        free(command_memory);
        return;
    }

    printf("\n============================================================\n");
    printf("              LINUX PROCESS LIFECYCLE\n");
    printf("============================================================\n");
    printf("Command : %s\n", input);

    /* CO1: process identification through Linux system calls. */
    printf("\n[CO1 - OS SERVICE / SYSTEM CALLS]\n");
    printf("Parent PID  : %d\n", getpid());
    printf("Parent PPID : %d\n", getppid());
    os_write("write() system call: starting process lifecycle...\n");

    log_message("\n=== New Process Lifecycle ===\n");
    log_message("Command: ");
    log_message(input);
    log_message("\n");

    /* CO3: anonymous pipe used by parent to send command text to child. */
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        free(command_memory);
        return;
    }

    printf("\n[CO3 - IPC USING PIPE]\n");
    printf("Anonymous pipe created between parent and child.\n");

    printf("\n[CO2 - PROCESS CREATION]\n");
    printf("Calling fork()...\n");

    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        free(command_memory);
        return;
    }

    if (pid == 0) {
        /* ---------------- CHILD ---------------- */
        close(pipefd[1]);

        char received[MAX_CMD];
        ssize_t bytes = read(pipefd[0], received, sizeof(received) - 1);
        close(pipefd[0]);

        if (bytes < 0) {
            perror("read from pipe");
            _exit(1);
        }
        received[bytes] = '\0';

        printf("\n[CO3 - CHILD RECEIVED IPC DATA]\n");
        printf("Child received command through pipe: %s\n", received);
        printf("Child PID  : %d\n", getpid());
        printf("Child PPID : %d\n", getppid());

        log_message("Child created successfully.\n");

        printf("\n[CO2 - PROGRAM EXECUTION]\n");
        printf("Child is calling execvp()...\n");
        printf("The child process image will now be replaced by the requested program.\n\n");
        fflush(stdout);

        /* execvp() replaces the child process with the requested command. */
        execvp(args[0], args);

        /* Reached only if execvp() fails. */
        perror("execvp");
        _exit(127);
    }

    /* ---------------- PARENT ---------------- */
    close(pipefd[0]);

    printf("Child PID   : %d\n", pid);
    printf("Child PPID  : %d\n", getpid());

    /* Send the command to the child through the pipe. */
    if (write(pipefd[1], input, strlen(input)) == -1) {
        perror("write to pipe");
    }
    close(pipefd[1]);

    log_message("Parent sent command to child through pipe.\n");

    /* CO6: start monitoring thread in the parent. */
    pthread_t monitor;
    int monitor_created = 0;
    pthread_mutex_lock(&report_mutex);
    child_running = 1;
    monitored_child = pid;
    pthread_mutex_unlock(&report_mutex);

    if (pthread_create(&monitor, NULL, monitor_thread, NULL) != 0) {
        perror("pthread_create");
        pthread_mutex_lock(&report_mutex);
        child_running = 0;
        pthread_mutex_unlock(&report_mutex);
    } else {
        monitor_created = 1;
    }

    printf("\n[CO2 - PARENT SYNCHRONIZATION]\n");
    printf("Parent is calling waitpid() for child %d...\n", pid);

    int status;
    pid_t result = waitpid(pid, &status, 0);

    pthread_mutex_lock(&report_mutex);
    child_running = 0;
    pthread_mutex_unlock(&report_mutex);

    if (result == -1) {
        perror("waitpid");
    } else {
        printf("waitpid() returned child PID: %d\n", result);
        printf("\n[CO2 - PROCESS TERMINATION / EXIT STATUS]\n");
        print_status(status);

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("Result: command executed successfully.\n");
        } else {
            printf("Result: command did not complete normally.\n");
        }
    }

    if (monitor_created) {
        pthread_join(monitor, NULL);
    }

    /* CO5: save the final lifecycle information to a file. */
    printf("\n[CO5 - FILE SYSTEM / FILE I/O]\n");
    printf("Saving lifecycle information to %s...\n", LOG_FILE);

    char report[512];
    if (WIFEXITED(status)) {
        snprintf(report, sizeof(report),
                 "Parent PID=%d, Child PID=%d, Command=\"%s\", Exit Status=%d\n",
                 getpid(), pid, input, WEXITSTATUS(status));
    } else {
        snprintf(report, sizeof(report),
                 "Parent PID=%d, Child PID=%d, Command=\"%s\", Child did not exit normally\n",
                 getpid(), pid, input);
    }
    log_message(report);
    printf("Lifecycle information written using open(), write(), and close().\n");

    /* CO4: release dynamically allocated memory. */
    free(command_memory);
    printf("\n[CO4 - VIRTUAL MEMORY]\n");
    printf("Command memory released using free().\n");

    printf("\n============================================================\n");
    printf("             COMPLETE LIFECYCLE FINISHED\n");
    printf("============================================================\n\n");
}

int main(void)
{
    char command[MAX_CMD];

    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("============================================================\n");
    printf("      LINUX PROCESS CREATION, EXECUTION AND TERMINATION\n");
    printf("============================================================\n");
    printf("Enter a Linux command to observe its complete process lifecycle.\n");
    printf("Examples: ls -l, pwd, date, whoami\n");
    printf("Type exit to quit.\n\n");

    while (1) {
        printf("Enter command: ");

        if (fgets(command, sizeof(command), stdin) == NULL)
            break;

        command[strcspn(command, "\n")] = '\0';

        if (strcmp(command, "exit") == 0)
            break;

        if (strlen(command) == 0) {
            printf("Please enter a command.\n");
            continue;
        }

        run_lifecycle(command);
    }

    printf("Program terminated.\n");
    pthread_mutex_destroy(&report_mutex);
    return 0;
}

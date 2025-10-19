#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <vector>

pid_t shell_pid = -1;

void launch_shell(const char *arg) {
    shell_pid = fork();
    if (shell_pid < 0) {
        perror("fork");
    } else if (shell_pid == 0) {
        std::vector<char*> bash_argv = { strdup("/usr/bin/bash"), strdup(arg), nullptr };
        if (execvp(bash_argv[0], bash_argv.data()) < 0)
            perror("execvp");
    }
}

int main(int argc, char *argv[]) {
    setenv("TERM", "linux", 1);
    setenv("USER", "root", 1);
    setenv("HOME", "/root", 1);
    setenv("PATH", "/usr/local/bin:/usr/bin", 1);

    struct sigaction signal_handler = {};
    signal_handler.sa_handler = [] (int) {
        pid_t pid;
        do {
            pid = waitpid(-1, nullptr, WNOHANG);
            if (pid == shell_pid)
                shell_pid = -1;
        } while (pid > 0);
    };
    sigaction(SIGCHLD, &signal_handler, nullptr);

    if (argc == 2 && strcmp(argv[1], "--startwm") == 0)
        launch_shell("/usr/bin/startwm");
    else
        launch_shell("-l");

    sigset_t sigset;
    sigprocmask(SIG_UNBLOCK, nullptr, &sigset);

    while (true) {
        sleep(100000);
        if (shell_pid == -1) {
            printf("init: initial shell died, dropping to new shell\n");
            launch_shell("-l");
        }
    }

    return 0;
}

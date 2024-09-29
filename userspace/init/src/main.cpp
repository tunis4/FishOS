#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    setenv("TERM", "linux", 1);
    setenv("USER", "root", 1);
    setenv("HOME", "/root", 1);
    setenv("PATH", "/usr/local/bin:/usr/bin", 1);

    if (argc == 2 && strcmp(argv[1], "--startwm") == 0) {
        int pid = fork();
        if (pid == 0) {
            char *bash_argv[] = { strdup("/usr/bin/bash"), strdup("/usr/bin/startwm"), NULL };
            if (execvp(bash_argv[0], bash_argv) < 0)
                perror("execvp");
        } else {
            while (true)
                sleep(1000);
        }
    } else {
        char *bash_argv[] = { strdup("/usr/bin/bash"), strdup("-l"), NULL };
        if (execvp(bash_argv[0], bash_argv) < 0)
            perror("execvp");
    }

    return 0;
}

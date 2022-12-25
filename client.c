#include "utils.h"
#include <errno.h>
#include <fcntl.h>
#include <resolv.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

char art[] = ""
"       /$$$$$$$           /$$                                      \n"
"      | $$__  $$         | $$                                      \n"
"      | $$  \\ $$ /$$$$$$ | $$  /$$$$$$                            \n"
"      | $$$$$$$//$$__  $$| $$ /$$__  $$                            \n"
"      | $$____/| $$  \\ $$| $$| $$$$$$$$                           \n"
"      | $$     | $$  | $$| $$| $$_____/                            \n"
"      | $$     |  $$$$$$/| $$|  $$$$$$$                            \n"
"      |__/      \\______/ |__/ \\_______/                          \n"
"        /$$$$$$  /$$                       /$$                     \n"
"       /$$__  $$| $$                      | $$                     \n"
"      | $$  \\__/| $$$$$$$  /$$   /$$  /$$$$$$$  /$$$$$$   /$$$$$$$\n"
"      | $$      | $$__  $$| $$  | $$ /$$__  $$ /$$__  $$ /$$_____/ \n"
"      | $$      | $$  \\ $$| $$  | $$| $$  | $$| $$$$$$$$|  $$$$$$ \n"
"      | $$    $$| $$  | $$| $$  | $$| $$  | $$| $$_____/ \\____  $$\n"
"      |  $$$$$$/| $$  | $$|  $$$$$$/|  $$$$$$$|  $$$$$$$ /$$$$$$$/ \n"
"       \\______/ |__/  |__/ \\______/  \\_______/ \\_______/|_______/\n";


#define SIZE 1024

int socket_fd = -1;

void sigTerm(int sig) {
	printf("Got signal to terminate client\n");
    if (socket_fd != -1) {
        close(socket_fd);
    }
    exit(0);
}

char* getline2(void) {
    char* line = malloc(100), * linep = line;
    size_t lenmax = 100, len = lenmax;
    int c;

    if (line == NULL)
        return NULL;

    for (;;) {
        c = fgetc(stdin);
        if (c == EOF)
            break;

        if (--len == 0) {
            len = lenmax;
            char* linen = realloc(linep, lenmax *= 2);

            if (linen == NULL) {
                free(linep);
                return NULL;
            }
            line = linen + (line - linep);
            linep = linen;
        }

        if ((*line++ = c) == '\n')
            break;
    }
    line--;
    *line = '\0';
    return linep;
}

int main(int argc, char* argv[]) {

    char buf[SIZE + 1];
    struct sockaddr_in servaddr;
    int port;

    struct sigaction action;

	action.sa_handler = sigTerm;
    sigfillset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    char* helper_text = "Usage %s -p <port> to start";

    if (argc != 3) {
        printf(helper_text, argv[0]);
        exit(1);
    }

    if (strcmp(argv[1], "-p") == 0) {
        int err = str2int(argv[2], &port);
        if (err) {
            perror("Wrong port shared to programm");
            exit(1);
        }
    } else {
        printf(helper_text, argv[0]);
        exit(1);
    }

    if ((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(1);
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    inet_aton("localhost", &(servaddr.sin_addr));

    int n;
    socklen_t len;

    printf("Starting connection...\n");
    if (connect(socket_fd, (const struct sockaddr*)&servaddr, sizeof(servaddr))) {
        fprintf(stderr, "Could not connect to server\n");
        exit(0);
    }

    fd_set master_fds;
    fd_set read_fds;
    int fdmax;
    int i, nbytes;

    FD_ZERO(&master_fds);
    FD_ZERO(&read_fds);
    
    FD_SET(socket_fd, &master_fds);
    FD_SET(STDIN_FILENO, &master_fds);

    fdmax = socket_fd;
    printf("%s\n\n", art);
    printf("Write your command (exit to stop):...\n");
    for(;;) {
        read_fds = master_fds;
        select(fdmax + 1, &read_fds, NULL, NULL, 0);

        for (i = 0; i <= fdmax; i++) {
            if (FD_ISSET(i, &read_fds)) {
                if (i == STDIN_FILENO) {
                    char* msg = getline2();

                    if (strncmp(msg, "exit", 4) == 0) {
                        printf("Bye ...\n");
                        close(socket_fd);
                        exit(0);
                        break;
                    }

                    int err = send(socket_fd, msg, strlen(msg), 0);
                    free(msg);
                    if (err == -1) {
                        printf("ERROR\n");
                    }
                }
                else if (i == socket_fd) {
                    if ((nbytes = recv(i, buf, SIZE, 0)) <= 0) {
                        if (nbytes == 0) {
                            printf("Connection closed from server\n");
                            close(socket_fd);
                            exit(0);
                        } else {
                            perror("recv");
                        }
                        close(i);
                        break;
                    } else {
                        buf[nbytes] = '\0';
                        printf("S: %s\n", buf);
                    }
                }
            }
        }
    }
    return 0;
}
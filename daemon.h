#ifndef DAEMON_H
#define DAEMON_H

#include "utils.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/msg.h>
#include <time.h>
#include <unistd.h>
#include <semaphore.h>
#include <regex.h>
#include <resolv.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/sem.h>

#define MAXLINE 1024
#define PATH_TO_CONFIG "daemon.config"
#define PATH_TO_QUESTIONS "game_questions.txt"
#define N 21

typedef struct daemonConfiguration {
	int port;
	char *logfile;
} daemonConfiguration;

enum ConfigUpdater {
	DEFAULT,
	PORT
};

typedef struct Game {
	char description[MAXLINE];
	char word[MAXLINE];
	char mask[MAXLINE];
	char cur_user;
	char password[MAXLINE];
	int score_1;
	int score_2;
	int cur_score;
	int is_finished;  // 0 - in progress, 1 - one player tried to use answer, 2 - finished, -1 - not started
} Game;

void sigChild(int);
void sigHup(int);
void sigTerm(int);
int Daemon(void);
void resetToDefaults();

// Configuration functions
int setUpConfig();
int updateConfig(enum ConfigUpdater action, int port, char* logfile);

#endif
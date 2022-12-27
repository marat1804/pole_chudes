#include "daemon.h"

// Sockets for players
int player1 = -1, player2 = -1;

//Configs 
daemonConfiguration config = { 0 };

// File descriptors
int socket_fd = -1;
int msgqid = -1;
int log_fd = -1;

// PIDs
pid_t master = -1;
pid_t w1 = -1;
pid_t w2 = -1;
pid_t w3 = -1;

typedef struct Memory {
	int sem_id;
	int is_game_started;
	Game game;	
} Memory;

char server_name[255];

Memory* shared_memory = NULL;

struct sembuf sop_lock[2] = {
        0, 0, 0,
        0, 1, 0
};

struct sembuf sop_unlock[1] = {
        0, -1, 0
};

void append_time_to_log_name() {
    char nowstr[40];
    struct tm *timenow;

    time_t now = time(NULL);
    timenow = gmtime(&now);

    strftime(nowstr, sizeof(nowstr), "_%Y-%m-%d_%H-%M-%S", timenow);
    strcat(config.logfile, nowstr);
}

/*
	Locks
*/

void sighup_lock() {
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGHUP);
    sigprocmask(SIG_BLOCK, &sigset, NULL);
}

void sighup_unlock() {
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGHUP);
    sigprocmask(SIG_UNBLOCK, &sigset, NULL);
}

void shared_lock(){
    if (semop(shared_memory->sem_id, sop_lock, 2) == -1) {
        LOG("Unable to lock shared memory", log_fd);
        perror("LOCK");
        exit(1);
    }
    LOG("Locked shared memory", log_fd);
}

void shared_unlock() {
    if (semop(shared_memory->sem_id, sop_unlock, 1) == -1) {
        LOG("Unable to unlock shared memory", log_fd);
        exit(1);
    }
    LOG("Unlocked shared memory", log_fd);
}

/*
	Control server state
*/

void resetGame() {
    shared_lock();
	shared_memory->game.cur_score = 0;
	shared_memory->game.cur_user = -1;
	memset(shared_memory->game.description, 0, MAXLINE);
	memset(shared_memory->game.mask, 0, MAXLINE);
	memset(shared_memory->game.word, 0, MAXLINE);
	memset(shared_memory->game.password, 0, MAXLINE);
	shared_memory->game.score_1 = 0;
	shared_memory->game.score_2 = 0;
	shared_memory->is_game_started = 0;
	shared_memory->game.is_finished = -1;
    shared_memory->game.game_owner = -1;
    shared_unlock();
}

void resetToDefaults() {
	LOG("Resetting the settings...", log_fd);

	if (socket_fd != -1) {
		close(socket_fd);
		LOG("Socket closed", log_fd);
	}
	if (msgqid != -1) {
		int res_cntl = msgctl(msgqid, IPC_RMID, 0);
		if (res_cntl == -1) {
			ERROR("Failed to remove message queue", log_fd);
		}
		LOG("Message queue removed", log_fd);
	}
    key_t sm_key;
	int sm_id;

	sm_key = ftok(server_name, 'M');
	sm_id = shmget(sm_key, sizeof(Memory), IPC_CREAT | 0666);
	if (sm_id < 0) {
		ERROR("Failed to create shared memory", log_fd);
		exit(1);
	}

	shared_memory = (Memory *)shmat(sm_id, (void*)0, 0);
    resetGame();

	shmdt((void*)shared_memory);
	LOG("Server cleaned", log_fd);

	if (log_fd != -1) {
		close(log_fd);
	}
}

int updateConfig(enum ConfigUpdater action, int port, char* logfile) {
	if (action == PORT) {
        if (socket_fd != -1) { close(socket_fd); }

        struct sockaddr_in name;
        socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        name.sin_family = AF_INET;
        name.sin_addr.s_addr = htonl(INADDR_ANY);
        name.sin_port = htons(port);
        int yes = 1;
        setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
        if (bind(socket_fd, (const struct sockaddr*)&name, sizeof(name)) == -1) {
            perror("Failed to bind to socket");
            return 1;
        }
        listen(socket_fd, 20);
        config.port = port;
		LOG("Port in config was updated", log_fd);
    }

    if (msgqid != -1) {
		int res_cnlt = msgctl(msgqid, IPC_RMID, 0);
        if (res_cnlt == -1) {
            perror("Removing of queue failed");
            return 1;
        }
    }
    close(log_fd);
    free(config.logfile);

    config.logfile = calloc(strlen(logfile) + 22, sizeof(char));
    strcpy(config.logfile, logfile);
	append_time_to_log_name();

    log_fd = open(config.logfile, O_WRONLY | O_APPEND | O_CREAT, 0660);
    if (log_fd == -1) {
        perror("Failed to get descriptor for logfile");
        return 1;
    }
    else {
        LOG("Log file was initialized", log_fd);
    }

    // Start to update message queue

    key_t key = ftok(server_name, 'M');
    if (key == -1) {
        perror("Failed to get key");
        return 1;
    }
    msgqid = msgget(key, IPC_CREAT | IPC_EXCL | 0660);
    
	if (msgqid == -1) {
        perror("Failed to get queue id");
        return 1;
    }

	char pattern[] = "From 'updateConfig': msgqid = %d";
    char result[30] = {0};
    sprintf(
        result,
        pattern,
        msgqid
    );
    LOG(result, log_fd);
    return 0;
}

int setUpConfig() {
	FILE* config_file;

    if ((config_file = fopen(PATH_TO_CONFIG, "r+")) == NULL) {
        perror("fopen");
        return 1;
    }

    int port;
    char* log_file;
    char buf[100] = { 0 };
    int action = DEFAULT;

    /*
     * Let's set port
     */
    if (fscanf(config_file, "port = %s\n", buf) != 1) {
        if (feof(config_file)) {
            return 1;
        }
        perror("fscanf");
        return 1;
    }
    int err = str2int(buf, &port);
    if (err) {
        perror("Failed to parse port");
        return 1;
    }

    // Let's check if we need to update config port
    if (config.port != port) {
        action += PORT;
    }

    /*
     * Let's set logfile path
     */
    memset(buf, 0, 100);

    if (fscanf(config_file, "log_path = %s", buf) != 1) {
        if (feof(config_file)) {
            return 1;
        }
        perror("fscanf");
        return 1;
    }

    fclose(config_file);
    int log_file_size = strlen(buf);
    log_file = calloc(log_file_size + 1, sizeof(char));
    if (!log_file) {
        perror("Out of memory");
        return 1;
    }
    strcpy(log_file, buf);

    err = 0;

    // Let's configuration update with action if needed
    err = updateConfig(action, port, log_file);
    free(log_file);
    char pattern[] = "My PID: %d";
    char result[20];
    sprintf(
        result,
        pattern,
        getpid()
    );
    LOG(result, log_fd);

    return err;
}

/*
	Signals
*/

void sigChild(int sig) {
	pid_t pid;
	int stat;

	while ((pid = waitpid(-1, &stat, WNOHANG)) > 0) {
		if (pid == master) { master = -1; }
		else if (pid == w1) { w1 = -1; }
		else if (pid == w2) { w2 = -1; }
		else if (pid == w3) { w3 = -1; }
	}
	return ;
}

void sigHup(int sig) {
	LOG("Got signal to update config", log_fd);
	setUpConfig();
	LOG("Config was updated", log_fd);
	return ;
}

void sigTerm(int sig) {
	LOG("Got signal to terminate server", log_fd);
	if (master != -1) { kill(master, 9); }
	if (w1 != -1) { kill(w1, 9); }
	if (w2 != -1) { kill(w2, 9); }
	if (w3 != -1) { kill(w3, 9); }

	char* answer_to_clients = "Server finished it's work";
	
    int len = strlen(answer_to_clients);
    if (player1 != -1) {
        send(player1, answer_to_clients, len, 0);
        close(player1);
    }
    if (player2 != -2) {
        send(player2, answer_to_clients, len, 0);
        close(player2);
    }

	resetToDefaults();
	exit(0);
}


/*
    Game funcs
*/

void generate_word_for_game(void) {
    LOG(":generator: Generating question for game", log_fd);
    int line_number = get_file_line_number(PATH_TO_QUESTIONS);
    if (line_number == 0) {
        ERROR(":generator: Invalid question file", log_fd);
        return ;
    }
    char pattern[] = ":generator :Question file has %d questions";
    char result[60] = {0};
    sprintf(
        result,
        pattern,
        line_number
    );
    LOG(result, log_fd);
    
    u_int8_t question_number = random_number(line_number);
    char pattern2[] = ":generator: Line for question - %d";
    char result2[60] = {0};
    sprintf(result2, pattern2, question_number + 1);
    
    char *line = get_nth_line_from_file(PATH_TO_QUESTIONS, question_number);
    if (!line) {
        ERROR(":generator: Invalid file", log_fd);
        return ;
    }

    int separator_index = -1, i;

    for (i = 0; i < strlen(line); ++i) {
        if (line[i] == ':') {
            separator_index = i;
            break;
        }
    }

    if (separator_index != -1){
        strncpy(shared_memory->game.description, line, separator_index);
        strcpy(shared_memory->game.word, line + separator_index + 1);
    } else {
        ERROR(":generator: Invalid question file. Format - question:answer", log_fd);
        free(line);
        return ;
    }
    free(line);
    LOG(":generator: Question and answer for game", log_fd);
    LOG(shared_memory->game.description, log_fd);
    LOG(shared_memory->game.word, log_fd);
    memset(shared_memory->game.mask, 0, MAXLINE);
    shared_memory->game.is_finished = 0;
    shared_memory->game.cur_user = random_number(2);
}

char *get_question_with_masked_answer(void) {
    char *result = (char *)calloc(MAXLINE, sizeof(char));
    if (!result) {
        ERROR("Not enough memory", log_fd);
        return NULL;
    }
    strcpy(result, "Question -> ");
    strcat(result, shared_memory->game.description);
    strcat(result, "\n");
    int cur_pointer = strlen(result), i;
    char letter, mask;
    for (i = 0; i < strlen(shared_memory->game.word); ++i) {
        letter = shared_memory->game.word[i];
        mask = shared_memory->game.mask[i];
        result[cur_pointer] = mask == 1 ? letter : '#';
        ++cur_pointer;
    }

    strcat(result, "\n");
    strcat(result, "Points this round ->");
    sprintf(result, "%s %d\n", result, shared_memory->game.cur_score);
    result[strlen(result)] = '\0';
    return result;
}

char *get_final_result() {
    char *result = (char*)calloc(MAXLINE, sizeof(char));
    if (!result) {
        ERROR("Not enough memory", log_fd);
        return NULL;
    }
    strcpy(result, "Game over!\n");
    strcat(result, shared_memory->game.cur_user == 0 ? "Player 1" : "Player 2");
    strcat(result, " has won!\n");
    char mini_result[100] = {0};
    sprintf(
        mini_result, 
        "Total points earned - %d\n",
        shared_memory->game.cur_user == 0 ? shared_memory->game.score_1 : shared_memory->game.score_2
    );
    strcat(result, mini_result);

    strcat(result, "\n\n To start new game enter: start {password}\n");
    return result;
}

void generate_current_score(int first) {
    shared_lock();
    int current_score = random_number(N) * 10;
    if (first) {
        if (current_score == 0) {
            current_score += 10;
        }
    }
    shared_memory->game.cur_score = current_score;
    shared_unlock();
}

char *get_bankrot_message() {
    char *result = (char*)calloc(MAXLINE, sizeof(char));
    if (!result) {
        ERROR("Not enough memory", log_fd);
        return NULL;
    }
    strcat(result, shared_memory->game.cur_user == 0 ? "Player 1" : "Player 2");
    strcat(result, " got bankroted!\n");
    strcat(result, "Move change!\n");
    char mini_result[100] = {0};
    sprintf(
        mini_result, 
        "Points this round ->%d\n",
        shared_memory->game.cur_score
    );
    strcat(result, mini_result);

    return result;
}

/*
	Daemon
*/

char master_answer[MAXLINE + 1] = {0};

int Daemon(void) {

	struct sigaction action;

	action.sa_handler = sigHup;
    sigfillset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGHUP, &action, NULL);
    action.sa_handler = sigTerm;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGKILL, &action, NULL);
	action.sa_handler = sigChild;
	sigaction(SIGCHLD, &action, NULL);

	if (setUpConfig()) {
		perror("Failed to set up config");
		exit(1);
	}

	key_t sm_key;
	int sm_id;

	sm_key = ftok(server_name, 'M');
	sm_id = shmget(sm_key, sizeof(Memory), IPC_CREAT | 0666);

	if (sm_id < 0) {
		printf("Failed to create shared memory\n");
		exit(1);
	}

	shared_memory = (Memory *)shmat(sm_id, (void*)0, 0);
	if ((int) shared_memory == -1) {
		printf("Failed to get shared memory\n");
		exit(1);
	}

	LOG("Server attached the memory to its virtual space...", log_fd);

	if ((shared_memory->sem_id = semget(sm_key, 1, IPC_CREAT | 0666)) == -1) {
        ERROR("Failed to get semaphore ID", log_fd);
    }
    LOG("Got semaphore ID", log_fd);

	struct sockaddr_in clientaddr;
	unsigned int client_len;

    struct sockaddr cl_addr;
    unsigned int cl_len;

	fd_set master_fds;    // master file descriptor list
    fd_set read_fds;  // temp file descriptor list for select()
    int fdmax;        // maximum file descriptor number
    int i;
    int newfd;        // newly accept()ed socket descriptor
    char buf[MAXLINE + 1];    // buffer for client data
    int nbytes;

    FD_ZERO(&master_fds);    // clear the master and temp sets
    FD_ZERO(&read_fds);

	FD_SET(socket_fd, &master_fds);
    fdmax = socket_fd;
	// Start
	LOG("Starting listeinig", log_fd);
	for (;;) {
		read_fds = master_fds; // copy it
        if (select(fdmax + 1, &read_fds, NULL, NULL, 0) == -1) {
			LOG("Error in select", log_fd);
			if (errno == EINTR) {
				continue;
			}
            perror("select");
            exit(4);
        }
        // run through the existing connections looking for data to read
        for(i = 0; i <= fdmax; i++) {
            if (FD_ISSET(i, &read_fds)) {
                if (i == socket_fd) {
					LOG("Got new connection", log_fd);
					newfd = accept(socket_fd, (struct sockaddr*)&clientaddr, &client_len);
					if (newfd == -1) {
                        perror("accept");
                    } else {
                       
                        LOG("New connection established", log_fd);
                        if (player1 == -1) {
                            player1 = newfd;
                            FD_SET(newfd, &master_fds); // add to master set
                            if (newfd > fdmax) {    // keep track of the max
                                fdmax = newfd;
                            }
                        }
                        else if (player2 == -1) {
                            player2 = newfd;
                            FD_SET(newfd, &master_fds); // add to master set
                            if (newfd > fdmax) {    // keep track of the max
                                fdmax = newfd;
                            }
                        }
                        else {
                            send(newfd, "Too much users on the server.", 30, 0);
                            close(newfd);
                            // Send message about to much players
                        }

						// Add blocks to acceess memory
                    }
                } else {
                    if ((nbytes = recv(i, buf, MAXLINE, 0)) <= 0) {
                        // got error or connection closed by client
                        if (nbytes == 0) {
                            // connection closed
                            LOG("Connection closed from user", log_fd);
                            if (i == player1) { player1 = -1; }
                            if (i == player2) { player2 = -1; }

                        } else {
                            perror("recv");
                        }
                        close(i); 
                        FD_CLR(i, &master_fds);
                        resetGame();
                        if (player1 != -1) {
                            send(player1, "User disconnected. Game was resetted", 37, 0);
                        }
                        if (player2 != -1) {
                            send(player2, "User disconnected. Game was resetted", 37, 0);
                        }
                        LOG("Reseting the game", log_fd);
                    } else {
                        buf[nbytes] = '\0';
                        char pattern[] = "Recived message from player %d";
                        char result[60] = {0};
                        sprintf(
                            result,
                            pattern,
                            i == player1 ? 1 : 2
                        );
                        LOG(result, log_fd);
                        LOG(buf, log_fd);
                        pid_t pid, pid_2, pid_3;

                        send_msg_to_msgq(buf, msgqid, msgqid, 1);
                        sighup_lock();
                        if ((pid = fork()) == 0) {
                            char *command = get_msg_from_msgq(msgqid, 1);
                            char second_arg[MAXLINE] = {0};
                            char letter;
                            int failed = 0;

                            if (sscanf(command, "join %s", second_arg)) {
                                if (strlen(second_arg) == 0) {
                                    failed = 1;
                                    LOG(":checker: Incorrect argument for command join", log_fd);
                                }
                            }
                            else if (sscanf(command, "answer %s", second_arg)) {
                                if (strlen(second_arg) == 0) {
                                    failed = 2;
                                    LOG(":checker: Incorrect argument for command answer", log_fd);
                                }
                            }
                            else if (sscanf(command, "start %s", second_arg)) {
                                if (strlen(second_arg) == 0) {
                                    failed = 3;
                                    LOG(":checker: Incorrect argument for command start", log_fd);
                                }
                            }
                            else if (sscanf(command, "letter %c", &letter)) {
                                if (letter == 0) {
                                    failed = 4;
                                    LOG(":checker: Incorrect argument for command letter", log_fd);
                                } 
                            }
                            else {
                                failed = 5;
                                LOG(":checker: Incorrect command", log_fd);
                            }

                            if (failed != 0) {
                                LOG(":checker: Command validation failed", log_fd);
                                memset(master_answer, 0, sizeof(master_answer));
                                switch (failed) {
                                    case 1:
                                        // failed = 1 - wrong argument for command join
                                        snprintf(master_answer, sizeof(master_answer), "Sorry, incorrect command join. Usage: join {password}.");
                                        break;
                                    case 2:
                                        // failed = 2 - wrong argument for command answer
                                        snprintf(master_answer, sizeof(master_answer), "Sorry, incorrect command join. Usage: answer {word}.");
                                        break;
                                    case 3:
                                        // failed = 3 - wrong argument for command start
                                        snprintf(master_answer, sizeof(master_answer), "Sorry, incorrect command start. Usage: start {password}.");
                                        break;
                                    case 4:
                                        // failed = 4 - worng argument for command letter
                                        snprintf(master_answer, sizeof(master_answer), "Sorry, incorrect command letter. Usage: letter {letter}.");
                                        break;
                                    default:
                                        snprintf(master_answer, sizeof(master_answer), "Sorry, incorrect command. All command list: join, answer, start, letter.");
                                        break;
                                }
                            } else {
                                LOG(":checker: Got valid command", log_fd);
                                snprintf(master_answer, sizeof(master_answer), "OK!");
                            }

                            free(command);
                            if (send_msg_to_msgq(master_answer, msgqid, msgqid, 2)) {
                                ERROR(":checker: Failed to send message\n ** Rollback **", log_fd);
                                resetToDefaults();
                            }
                            memset(master_answer, 0, sizeof(master_answer));
                            LOG(":checker: Sent answer to master", log_fd);
                            exit(0);
                        }
                        else {
                            w1 = pid;
                        }
                        wait(NULL);
                        sighup_unlock();
                        char *worker_answer = get_msg_from_msgq(msgqid, 2);

                        LOG("Worker answer:", log_fd);
                        LOG(worker_answer, log_fd);
                        
                        if (strncmp(worker_answer, "Sorry", 5) == 0) {
                            send(i, worker_answer, strlen(worker_answer), 0);
                            LOG("Send answer to player", log_fd);
                            free(worker_answer);
                            continue;
                        }
                        free(worker_answer);
                        // Command processing
                        
                        char player_index[20] = {0};
                        sprintf(player_index, "%d", i);

                        send_msg_to_msgq(buf, msgqid, msgqid, 3);
                        send_msg_to_msgq(player_index, msgqid, msgqid, 31);
                        
                        sighup_lock();
                        if ((pid = fork()) == 0) {
                            memset(master_answer, 0, sizeof(master_answer));
                            char *command = get_msg_from_msgq(msgqid, 3);
                            char *from_fd = get_msg_from_msgq(msgqid, 31);
                            int from_index;
                            str2int(from_fd, &from_index);
                            free(from_fd);
                            char send_to_who[20] = {0};

                            char second_arg[MAXLINE] = {0};
                            char letter;
                            if (sscanf(command, "join %s", second_arg)) {
                                LOG(":handler: Processing command - join", log_fd);
                                if (strlen(shared_memory->game.password) != 0) { // Can join
                                        // Check password
                                    if (shared_memory->game.game_owner != from_index) {
                                        if (strcmp(second_arg, shared_memory->game.password) == 0) { // OK Password
                                            LOG(":hander: Correct password got", log_fd);
                                            snprintf(master_answer, 
                                                sizeof(master_answer), 
                                                "Starting game!");
                                            snprintf(send_to_who, sizeof(send_to_who), "Both");
                                        }
                                        else { // Wrong password
                                            LOG(":handler: Wrong password got", log_fd);
                                            snprintf(master_answer, 
                                                sizeof(master_answer), 
                                                "Wrong password. Try again");
                                            snprintf(send_to_who, sizeof(send_to_who), "One");
                                        }
                                    } else {
                                        LOG("Try to join his own game", log_fd);
                                        snprintf(
                                            master_answer,
                                            sizeof(master_answer),
                                            "Can't join your own game!"
                                        );
                                        snprintf(send_to_who, sizeof(send_to_who), "One");
                                    }
                                }   
                                else { // No game to join
                                    LOG(":handler: No available game", log_fd);
                                    snprintf(master_answer, 
                                            sizeof(master_answer), 
                                            "No available games on server. You can create your game using start {password}");
                                    snprintf(send_to_who, sizeof(send_to_who), "One");
                                }                             
                            }
                            else if (sscanf(command, "answer %s", second_arg)) {
                                LOG(":handler: Processing command - answer", log_fd);
                                if (shared_memory->game.is_finished != -1) {
                                    int cur_user = shared_memory->game.cur_user == 0 ? player1 : player2;
                                    if (cur_user != from_index) { // Not user's turn
                                        LOG(":handler: Got not user's turn", log_fd);
                                        snprintf(master_answer, 
                                                sizeof(master_answer),
                                                "It's not your turn! Please wait");
                                        snprintf(send_to_who, sizeof(send_to_who), "One");
                                    } else { // User's turn
                                        LOG(":handler: Got user's turn", log_fd);
                                        if (strcmp(second_arg, shared_memory->game.word) == 0) { // Guessed
                                            LOG("User guessed", log_fd);
                                            shared_lock();
                                            if (cur_user == player1) {
                                                shared_memory->game.score_1 += shared_memory->game.cur_score;
                                            } else {
                                                shared_memory->game.score_2 += shared_memory->game.cur_score;
                                            }
                                            shared_memory->game.is_finished = 2;
                                            shared_unlock();
                                            snprintf(
                                                master_answer,
                                                sizeof(master_answer),
                                                "Player %d guessed the word!",
                                                cur_user == player1 ? 1 : 2
                                            );
                                        } else {
                                            LOG("User mistaken", log_fd);
                                            snprintf(
                                                master_answer,
                                                sizeof(master_answer),
                                                "Player %d mistaken the word! Game has ended for him :(",
                                                cur_user == player1 ? 1 : 2
                                            );
                                            shared_lock();
                                            shared_memory->game.is_finished = 1;
                                            shared_memory->game.cur_user = !shared_memory->game.cur_user;
                                            shared_unlock();
                                            generate_current_score(0);
                                        }
                                        snprintf(send_to_who, sizeof(send_to_who), "Both");
                                    }
                                }
                                else {
                                    snprintf(send_to_who, sizeof(send_to_who), "One");
                                    snprintf(master_answer, sizeof(master_answer), "There is no started game on server!");
                                }
                            }
                            else if (sscanf(command, "start %s", second_arg)) {
                                LOG(":handler: Processing command - start", log_fd);
                                if (strlen(shared_memory->game.password) != 0) { // There is a game on server
                                    snprintf(master_answer, 
                                            sizeof(master_answer), 
                                            "Sorry, there is a game on server. Use: join {password} to connect to the existing game.");
                                    LOG(":handler: Attemp to create another game", log_fd);
                                }
                                else { // No game -> creating
                                    shared_lock();
                                    strcpy(shared_memory->game.password, second_arg);
                                    shared_memory->game.game_owner = from_index;
                                    shared_unlock();
                                    LOG(":handler: Creating a new game with password", log_fd);
                                    LOG(second_arg, log_fd);
                                    snprintf(master_answer, 
                                            sizeof(master_answer), 
                                            "Game successfully created. Waiting for other players.");
                                }
                                snprintf(send_to_who, sizeof(send_to_who), "One");
                            }
                            else if (sscanf(command, "letter %c", &letter)) {
                                LOG(":handler: Processing command - letter", log_fd);
                                if (shared_memory->game.is_finished != -1) {
                                    int cur_user = shared_memory->game.cur_user == 0 ? player1 : player2;
                                    if (cur_user != from_index) { // Not user's turn
                                        LOG(":handler: Got not user's turn", log_fd);
                                        snprintf(master_answer, 
                                                sizeof(master_answer),
                                                "It's not your turn! Please wait");
                                        snprintf(send_to_who, sizeof(send_to_who), "One");
                                    } else { // User's turn
                                        LOG(":handler: Got user's turn", log_fd);
                                        int found_flag = 0, j;
                                        char current_letter, mask;
                                        shared_lock();
                                        for (j = 0; j < strlen(shared_memory->game.word); ++j) {
                                            current_letter = shared_memory->game.word[j];
                                            mask = shared_memory->game.mask[j];
                                            if ((current_letter == letter) & (mask == 0)) {
                                                found_flag = 1;
                                                shared_memory->game.mask[j] = 1;
                                            }
                                        }
                                        shared_unlock();
                                        if (found_flag) { // It s a guess and give him points
                                            LOG("User guessed", log_fd);
                                            shared_lock();
                                            if (cur_user == player1) {
                                                shared_memory->game.score_1 += shared_memory->game.cur_score;
                                            } else {
                                                shared_memory->game.score_2 += shared_memory->game.cur_score;
                                            }
                                            shared_unlock();
                                            // Now send message
                                            snprintf(
                                                master_answer,
                                                sizeof(master_answer),
                                                "Player %d guessed! Keep up the good work!",
                                                cur_user == player1 ? 1 : 2
                                            );
                                        }
                                        else {
                                            LOG("User mistaken", log_fd);
                                            if (shared_memory->game.is_finished != 1) {
                                                snprintf(
                                                    master_answer,
                                                    sizeof(master_answer),
                                                    "Player %d mistaken. Move changeover",
                                                    cur_user == player1 ? 1 : 2
                                                );
                                                shared_lock();
                                                shared_memory->game.cur_user = !shared_memory->game.cur_user;
                                                shared_unlock();
                                            }
                                            else {
                                                snprintf(master_answer, sizeof(master_answer), "No move change");
                                            }
                                        }
                                        generate_current_score(0);
                                        snprintf(send_to_who, sizeof(send_to_who), "Both");
                                    }
                                }
                                else {
                                    snprintf(send_to_who, sizeof(send_to_who), "One");
                                    snprintf(master_answer, sizeof(master_answer), "There is no started game on server!");
                                }
                            }
                            
                            free(command);
                            if (send_msg_to_msgq(master_answer, msgqid, msgqid, 4)) {
                                ERROR(":handler: Failed to send message\n ** Rollback **", log_fd);
                                resetToDefaults();
                            }
                            memset(master_answer, 0, sizeof(master_answer));
                            if (send_msg_to_msgq(send_to_who, msgqid, msgqid, 5)) {
                                ERROR(":handler: Failed to send message\n ** Rollback **", log_fd);
                                resetToDefaults();
                            }
                            LOG(":handler: Sent answer to master", log_fd);
                            exit(0);

                        }
                        else { 
                            w2 = pid; 
                        }
                        wait(NULL);
                        sighup_unlock();
                        char *handler_answer = get_msg_from_msgq(msgqid, 4);
                        char *send_to = get_msg_from_msgq(msgqid, 5);

                        LOG("Handler answer:", log_fd);
                        LOG(handler_answer, log_fd);
                        LOG("Sending to:", log_fd);
                        LOG(send_to, log_fd);

                        if (strncmp(handler_answer, "Starting game", 10) == 0) {
                            if ((pid = fork()) == 0) {
                                generate_word_for_game();
                                generate_current_score(1);
                            }
                            else {
                                w3 = pid;
                            }
                            wait(NULL);
                        }
 

                        if (strcmp(send_to, "Both") == 0) {
                            send(player1, handler_answer, strlen(handler_answer), 0);
                            send(player2, handler_answer, strlen(handler_answer), 0);
                            LOG("Send to both players", log_fd);
                            if ((shared_memory->game.is_finished == 0) || (shared_memory->game.is_finished == 1)) {
                                char *question = get_question_with_masked_answer();
                                if (question) {
                                    send(player1, question, strlen(question), 0);
                                    send(player2, question, strlen(question), 0);
                                    free(question);
                                    if (shared_memory->game.cur_score == 0) {
                                        LOG("User got bankroted", log_fd);
                                        generate_current_score(1);
                                        shared_lock();
                                        shared_memory->game.cur_user = !shared_memory->game.cur_user;
                                        shared_unlock();
                                        char *bnkrt = get_bankrot_message();
                                        if (bnkrt) {
                                            send(player1, bnkrt, strlen(bnkrt), 0);
                                            send(player2, bnkrt, strlen(bnkrt), 0);
                                            free(bnkrt);
                                        }
                                    }
                                    int cur_user = shared_memory->game.cur_user == 0 ? player1 : player2;
                                    int other_user = shared_memory->game.cur_user == 0 ? player2 : player1;
                                    send(cur_user, "It's your turn!", 16, 0);
                                    send(other_user, "It's others players turn", 25, 0);
                                    LOG("Sended question", log_fd);
                                }
                            }
                            else if (shared_memory->game.is_finished == 2) {
                                char *answer = get_final_result();
                                if (answer) {
                                    send(player1, answer, strlen(answer), 0);
                                    send(player2, answer, strlen(answer), 0);
                                    free(answer);
                                }
                                LOG("Send final message", log_fd);
                                resetGame();
                                LOG("Game resetted", log_fd);
                            }
                        }
                        else {
                            send(i, handler_answer, strlen(handler_answer), 0);
                            LOG("Send answer to player", log_fd);
                        }
                        free(handler_answer);
                        free(send_to);
                    }
                } 
            } 
        } 
	}
}

/*
	Main
*/

int main(int argc, char *argv[]) {
	pid_t parent_pid;

	char *helper_text = "To start program: %s -s\nTo run in interactive mode: %s -i\n";
	strcpy(server_name, argv[0]);
	if (argc < 2) {
		printf(helper_text, argv[0]);
		exit(1);
	}

	if (strcmp(argv[1], "-i") == 0) {
		Daemon();
	}
	else if (strcmp(argv[1], "-s") == 0) {
        if ((parent_pid = fork()) < 0) {
            printf("\nFailed to fork proccess");
            exit(1);
        }
        else if (parent_pid != 0) exit(0);
        setsid();
        Daemon();
	} else {
        printf(helper_text, argv[0]);
        exit(1);
    }

	printf("Initializing server daemon\n");

	return 0;
}


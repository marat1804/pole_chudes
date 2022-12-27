#ifndef UTILS_H
#define UTILS_H

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#define ERROR(mes, fd) writeToFile(mes, __FILE__, __LINE__, errno, fd)
#define LOG(mes, fd) writeLogToFile(mes, fd)

struct servermsg {
	long mtype; // message type
	int msgqid; // id of sender queue
	char mtext[1]; // message body
};

/*
	Converters
*/

int str2int(const char* str, int* result) {
	long int temp = 0;
	int sign = 1;
	const char* c = str;

	if (!str) {
		return -1;
	}

	if (!result) {
		return -1;
	}

	if (*c == '-') {
		sign = -1;
		c++;
	}

	for(; *c; c++) {
		if (*c < '0' || *c > '9') {
			return -1;
		}
		temp *= 10;
		temp += (long) (*c - '0');
		if (temp > INT_MAX) {
			return -1;
		}
	}
	*result = (int)temp * sign;
	return 0;
}

/*
	Read and write text to msg_queue
*/

char* read_msg_from_msgq(int msgqid, long type)
{
    int buflen = 200;
    int rcv_res;
    char* buf = (char*)calloc(sizeof(long) + buflen, sizeof(char));
    if (!buf) {
        perror("Not enough memory");
        return NULL;
    }
    for (;;) {
        errno = 0;
        rcv_res = msgrcv(msgqid, buf, buflen, type, IPC_NOWAIT);
        if (rcv_res > -1) {
            break;
        }
        if (errno == E2BIG) {
            buf = (char*)realloc(buf, sizeof(long) + buflen);
            if (!buf) {
                perror("Not enough memory");
                return NULL;
            }
            buflen += 10;
        }
        else if (errno == ENOMSG) {
            continue;
        }
        else {
            perror("msgrcv error");
            return NULL;
        }
    }
    buf[rcv_res] = '\0';
    return buf;
}

char* get_msg_from_msgq(int msgqid, long type)
{
    char* tmp = read_msg_from_msgq(msgqid, type);
    size_t len = strlen(((struct servermsg*)tmp)->mtext);
    char* res = (char *)malloc(len + 1);
    strncpy(res, ((struct servermsg*)tmp)->mtext, len);
    res[len] = '\0';
    free(tmp);
    return res;
}

int send_msg_to_msgq(char* msg, int msgqid, int senderid, long type)
{
    if (!msg) {
        errno = EINVAL;
        return -1;
    }
    int err = 0;
    int tmp2 = strlen(msg);
    int buflen = sizeof(senderid) + strlen(msg) + 10;
    char* buf = (char*)calloc(sizeof(long) + buflen, sizeof(char));
    if (!buf) {
        perror("Not enough memory");
        return -1;
    }
    struct servermsg* tmp = (struct servermsg*)buf;
    tmp->mtype = type;
    tmp->msgqid = senderid;
    strcpy(tmp->mtext, "");
    strcat(tmp->mtext, msg);
    err = msgsnd(msgqid, (void *)buf, buflen, 0);
    if (err) {
        perror("failed to send message");
        return -1;
    }
    free(buf);
    return 0;
}

/*
	Logs
*/

void writeToFile(const char* msg, const char* file, int line, int err, int fd) {
	char *buf = (char *)calloc(1024, sizeof(char));
	sprintf(buf, "[%s:%d]: %s (errno=%d, %s)\n", file, line, msg, err, strerror(err));
	write(fd, buf, strlen(buf));
	free(buf);
}

void writeLogToFile(const char* msg, int fd) {
	time_t now;
	struct tm* ptr;
	char tbuf[80];
	time(&now);
	ptr = localtime(&now);
	strftime(tbuf, 80, "%Y-%B-%e %H:%M:%S", ptr);
	char *buf = (char *)calloc(1024, sizeof(char));
	sprintf(buf, "[%s]: %s\n", tbuf, msg);
	write(fd, buf, strlen(buf));
	free(buf);
}

/*
    Random number
*/

u_int8_t random_number(int n) {
    int urandom_fd = open("/dev/urandom", O_RDONLY);
    if (urandom_fd == -1) {
        fprintf(stderr, "Cannot open /dev/urandom\n");
        return -1;
    }
    u_int8_t random_number;
    if (read(urandom_fd, &random_number, sizeof(u_int8_t)) == -1) {
        fprintf(stderr, "Cannot read from /dev/urandom\n");
        return -1;
    }
    if (close(urandom_fd) == -1) {
        fprintf(stderr, "Cannot close /dev/urandom\n");
        return -1;
    }
    return random_number % n;
}

/*
    Game file utils
*/

int get_file_line_number(char *filename)
{
    FILE *fp;
    int count = 0;  
    char c;  
 
    fp = fopen(filename, "r");
 
    if (fp == NULL)
    {
        printf("Could not open file %s", filename);
        return 0;
    }
    for (c = getc(fp); c != EOF; c = getc(fp))
        if (c == '\n') 
            count = count + 1;
    fclose(fp);
    return count;
}

char* get_nth_line_from_file(char *filename, int n) {
    FILE *file = fopen(filename, "r");
    int count = 0;

    char *result = (char *)calloc(1025, sizeof(char));
    if (!result) {
        return NULL;
    }
    if ( file != NULL )
    {
        char line[1024];
        while (fgets(line, sizeof line, file) != NULL)
        {
            if (count == n)
            {
                strcpy(result, line);
                result[strlen(line) - 1] = '\0';
                fclose(file);
                return result;
            }
            else
            {
                count++;
            }
        }
        fclose(file);
    }
    free(result);
    return NULL;
}

#endif
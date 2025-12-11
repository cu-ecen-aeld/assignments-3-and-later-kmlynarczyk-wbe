#include <stdio.h>
#include <syslog.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/sendfile.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/queue.h>
#include <time.h>

#ifndef SLIST_FOREACH_SAFE
#define	SLIST_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = SLIST_FIRST(head);				\
	    (var) && ((tvar) = SLIST_NEXT(var, field), 1);		\
	    (var) = (tvar))
#endif

static const char* out_filepath = "/var/tmp/aesdsocketdata";
static bool caught_signal = false;
static int server_fd = -1;

struct thread_data {
    pthread_t thread_id;
    pthread_mutex_t* out_file_mutex;
    int client_fd;
    bool completed;

    SLIST_ENTRY(thread_data) threads;
};

SLIST_HEAD(slisthead, thread_data);

static void *thread_start(void *thread_param) {
    struct thread_data *data = (struct thread_data *)thread_param;
    data->completed = false;

    syslog(LOG_INFO, "Thread #%ld started working", data->thread_id);

    char buffer[128 * 1024];
    char line_buffer[128 * 1024];
    size_t linepos = 0;

    while(!caught_signal) {
        ssize_t n = read(data->client_fd, buffer, sizeof(buffer) - 1);

        if(n < 0) {
            int tmp_errno = errno;
            if(tmp_errno == EINTR) {
                continue;
            }

            syslog(LOG_ERR, "Thread #%ld: Error %d (%s) on read()", data->thread_id, tmp_errno, strerror(tmp_errno));
            break;
        } else if(n == 0) {
            syslog(LOG_INFO, "Thread #%ld: Closed connection", data->thread_id);
            break;
        }

        int rc = pthread_mutex_lock(data->out_file_mutex);

        if(rc != 0) {
            syslog(LOG_ERR, "Thread #%ld: failed to lock mutex", data->thread_id);
            break;
        }

        FILE * out_file = fopen(out_filepath, "a+");

        if(out_file == NULL) {
            pthread_mutex_unlock(data->out_file_mutex);
            syslog(LOG_ERR, "Thread #%ld: failed to open file %s", data->thread_id, out_filepath);
            break;
        }

        for(ssize_t i = 0; i < n; ++i) {
            char c = buffer[i];

            if(c == '\n') {
                line_buffer[linepos] = '\0';
                fprintf(out_file, "%s\n", line_buffer);
                syslog(LOG_DEBUG, "Thread #%ld: Appended line: %s", data->thread_id, line_buffer);
                linepos = 0;
            } else {
                if(linepos < sizeof(line_buffer) - 1) {
                    line_buffer[linepos++] = c;
                } else {
                    syslog(LOG_ERR, "Thread #%ld: line_buffer overflow at %ld", data->thread_id, i);
                }
            }
        }

        if(out_file) {
            fflush(out_file);
            fclose(out_file);
            out_file = NULL;
        }

        int in_fd = open(out_filepath, O_RDONLY);

        if(in_fd < 0) {
            pthread_mutex_unlock(data->out_file_mutex);
            syslog(LOG_ERR, "Thread #%ld: failure on in_fd = open()", data->thread_id);
            break;
        }

        off_t offset = 0;
        struct stat st;

        if(fstat(in_fd, &st) < 0) {
            pthread_mutex_unlock(data->out_file_mutex);
            syslog(LOG_ERR, "Thread #%ld: Error %d (%s) on fstat()", data->thread_id, errno, strerror(errno));
            close(in_fd);
            break;
        }

        ssize_t sent;
        while (offset < st.st_size) {
            sent = sendfile(data->client_fd, in_fd, &offset, st.st_size - offset);
            if (sent < 0) {
                syslog(LOG_ERR, "Error %d (%s) on sendfile()", errno, strerror(errno));
                break;
            }
        }

        close(in_fd);

        rc = pthread_mutex_unlock(data->out_file_mutex);

        if(rc != 0) {
            syslog(LOG_ERR, "Thread #%ld: failed to unlock mutex", data->thread_id);
            break;
        }
    }

    syslog(LOG_INFO, "Thread #%ld finished working", data->thread_id);
    data->completed = true;
    return thread_param;
}

static void signal_handler(int signal_number) {
    caught_signal = (signal_number == SIGINT || signal_number == SIGTERM);

    if(caught_signal) {
        if(server_fd != -1) {
            close(server_fd);
            server_fd = -1;
        }
    }
}

static void timer_handler(union sigval sv) {
    pthread_mutex_t* out_file_mutex = (pthread_mutex_t *)sv.sival_ptr;

    time_t rawtime;
    struct tm *timeinfo;
    char buffer[128];

    time(&rawtime);
    timeinfo = localtime(&rawtime);

    strftime(buffer, sizeof(buffer), "timestamp:%a, %d %b %Y %T %z", timeinfo);

    pthread_mutex_lock(out_file_mutex);
    FILE * out_file = fopen(out_filepath, "a+");

    if(out_file == NULL) {
        pthread_mutex_unlock(out_file_mutex);
        syslog(LOG_ERR, "Failed to open file %s in timer_handler()", out_filepath);
        return;
    }

    fprintf(out_file, "%s\n", buffer);
    fflush(out_file);
    fclose(out_file);

    pthread_mutex_unlock(out_file_mutex);
}

static void start_timer(pthread_mutex_t* out_file_mutex) {
    timer_t timerid;
    struct sigevent sev;
    struct itimerspec its;

    memset(&sev, 0, sizeof(struct sigevent));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = timer_handler;
    sev.sigev_notify_attributes = NULL;
    sev.sigev_value.sival_ptr = out_file_mutex;

    if (timer_create(CLOCK_REALTIME, &sev, &timerid) == -1) {
        syslog(LOG_ERR, "Failed to create timer");
        return;
    }

    its.it_value.tv_sec = 10;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 10;
    its.it_interval.tv_nsec = 0;

    if (timer_settime(timerid, 0, &its, NULL) == -1) {
        syslog(LOG_ERR, "Failed to start timer");
    } else {
        syslog(LOG_INFO, "Started timer");
    }
}

int main(int argc, char** argv) {
    int ret_code = 0;
    int daemon_mode = 0;

    struct slisthead thread_list_head;
    SLIST_INIT(&thread_list_head);

    int opt;
    while ((opt = getopt(argc, argv, "d")) != -1) {
        if (opt == 'd') daemon_mode = 1;
    }

    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

    struct sigaction new_action;
    memset(&new_action, 0, sizeof(struct sigaction));
    new_action.sa_handler = signal_handler;

    if(sigaction(SIGINT, &new_action, NULL) != 0) {
        syslog(LOG_ERR, "Error %d (%s) registering for SIGINT", errno, strerror(errno));
        ret_code = 1;
        goto cleanup;
    }

    if(sigaction(SIGTERM, &new_action, NULL) != 0) {
        syslog(LOG_ERR, "Error %d (%s) registering for SIGTERM", errno, strerror(errno));
        ret_code = 1;
        goto cleanup;
    }

    int status;
    struct addrinfo hints;
    struct addrinfo *servinfo = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if((status = getaddrinfo(NULL, "9000", &hints, &servinfo)) != 0) {
        syslog(LOG_ERR, "Error %d (%s) on getaddrinfo()", errno, strerror(errno));
        ret_code = -1;
        goto cleanup;
    }

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if(server_fd == -1) {
        syslog(LOG_ERR, "Error %d (%s) on socket()", errno, strerror(errno));
        ret_code = -1;
        goto cleanup;
    }

    int yes = 1;
    if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
        syslog(LOG_ERR, "Error %d (%s) on setsockopt()", errno, strerror(errno));
        ret_code = -1;
        goto cleanup;
    }

    if(bind(server_fd, servinfo->ai_addr, sizeof(struct sockaddr)) != 0) {
        syslog(LOG_ERR, "Error %d (%s) on bind()", errno, strerror(errno));
        ret_code = -1;
        goto cleanup;
    }

    if(daemon_mode) {
        pid_t pid = fork();

        if(pid < 0) {
            syslog(LOG_ERR, "Error %d (%s) on fork()", errno, strerror(errno));
            ret_code = -1;
            goto cleanup;
        }

        if(pid > 0) {
            syslog(LOG_INFO, "The process ID of child is %d.", pid);
            exit(0);
        }
    }

    pthread_mutex_t out_file_mutex;
    pthread_mutex_init(&out_file_mutex, NULL);

    start_timer(&out_file_mutex);

    if(listen(server_fd, 100) < 0) {
        syslog(LOG_ERR, "Error %d (%s) on listen()", errno, strerror(errno));
        ret_code = -1;
        goto cleanup;
    }

    syslog(LOG_INFO, "Listening.");

    while(!caught_signal) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        syslog(LOG_INFO, "Waiting for connection request.");

        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

        if(client_fd < 0) {
            if(errno == EINTR) {
                break;
            }

            syslog(LOG_ERR, "Error %d (%s) on accept()", errno, strerror(errno));
            ret_code = -1;
            goto cleanup;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

        syslog(LOG_INFO, "Accepted connection from %s", ip);

        struct thread_data *data, *tmp;

        SLIST_FOREACH_SAFE(data, &thread_list_head, threads, tmp) {
            if(!data->completed) {
                continue;
            }

            syslog(LOG_INFO, "Joining thread #%ld", data->thread_id);
            int join_rc = pthread_join(data->thread_id, NULL);

            if(join_rc != 0) {
                syslog(LOG_ERR, "Failed to join thread #%ld", data->thread_id);
            } else {
                syslog(LOG_INFO, "Joined thread #%ld", data->thread_id);
            }

            SLIST_REMOVE(&thread_list_head, data, thread_data, threads);
            free(data);
        }

        data = malloc(sizeof(struct thread_data));
        data->client_fd = client_fd;
        data->out_file_mutex = &out_file_mutex;
        data->completed = false;
        SLIST_INSERT_HEAD(&thread_list_head, data, threads);

        int thread_create_rc = pthread_create(&data->thread_id, NULL, thread_start, data);

        if(thread_create_rc != 0) {
            syslog(LOG_ERR, "Failed to create thread");
        }
    }

cleanup:
    if(caught_signal) {
        syslog(LOG_INFO, "Caught signal, exiting");
        remove(out_filepath);
    }

    while(!SLIST_EMPTY(&thread_list_head)) {
        struct thread_data* data = SLIST_FIRST(&thread_list_head);

        if(!data->completed && data->client_fd != -1) {
            if(close(data->client_fd) != 0) {
                syslog(LOG_ERR, "Thread #%ld: Error %d (%s) on client_id close()", data->thread_id, errno, strerror(errno));
            }
        }

        syslog(LOG_INFO, "Joining thread #%ld", data->thread_id);
        int join_rc = pthread_join(data->thread_id, NULL);

        if(join_rc != 0) {
            syslog(LOG_ERR, "Failed to join thread #%ld", data->thread_id);
        } else {
            syslog(LOG_INFO, "Joined thread #%ld", data->thread_id);
        }

        SLIST_REMOVE_HEAD(&thread_list_head, threads);
        free(data);
    }
    SLIST_INIT(&thread_list_head);

    if(server_fd != -1) {
        if(close(server_fd) != 0) {
            syslog(LOG_ERR, "Error %d (%s) on server_fd close()", errno, strerror(errno));
        }
    }

    pthread_mutex_destroy(&out_file_mutex);

    if(servinfo) {
        freeaddrinfo(servinfo);
    }

    syslog(LOG_INFO, "Program finished");

    closelog();
    return ret_code;
}

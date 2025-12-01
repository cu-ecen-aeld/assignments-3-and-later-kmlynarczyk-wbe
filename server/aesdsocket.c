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

static bool caught_signal = false;
static int server_fd = -1;
static int client_fd = -1;

static void signal_handler(int signal_number) {
    caught_signal = (signal_number == SIGINT || signal_number == SIGTERM);

    if(caught_signal) {
        if(client_fd != -1) {
            close(client_fd);
            client_fd = -1;
        }

        if(server_fd != -1) {
            close(server_fd);
            server_fd = -1;
        }
    }
}

int main(int argc, char** argv) {
    int ret_code = 0;
    const char* out_filepath = "/var/tmp/aesdsocketdata";
    FILE *out_file = NULL;
    char buffer[128 * 1024];
    char line_buffer[128 * 1024];
    ssize_t n;
    size_t linepos = 0;
    int daemon_mode = 0;

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

        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

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

        while(!caught_signal) {
            n = read(client_fd, buffer, sizeof(buffer) - 1);

            if(n < 0) {
                int tmp_errno = errno;
                if(tmp_errno == EINTR) {
                    continue;
                }

                syslog(LOG_ERR, "Error %d (%s) on read()", tmp_errno, strerror(tmp_errno));
                ret_code = -1;
                break;
            } else if(n == 0) {
                syslog(LOG_INFO, "Closed connection from %s", ip);
                break;
            }

            out_file = fopen(out_filepath, "a+");

            if(out_file == NULL) {
                syslog(LOG_ERR, "failed to open file %s", out_filepath);
                ret_code = -1;
                goto cleanup;
            }

            for(ssize_t i = 0; i < n; ++i) {
                char c = buffer[i];

                if(c == '\n') {
                    line_buffer[linepos] = '\0';
                    fprintf(out_file, "%s\n", line_buffer);
                    syslog(LOG_DEBUG, "Appended line: %s", line_buffer);
                    linepos = 0;
                } else {
                    if(linepos < sizeof(line_buffer) - 1) {
                        line_buffer[linepos++] = c;
                    } else {
                        syslog(LOG_ERR, "line_buffer overflow at %ld", i);
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
                ret_code = -1;
                goto cleanup;
            }

            off_t offset = 0;
            struct stat st;

            if(fstat(in_fd, &st) < 0) {
                syslog(LOG_ERR, "Error %d (%s) on fstat()", errno, strerror(errno));
                close(in_fd);
                ret_code = -1;
                goto cleanup;
            }

            ssize_t sent;
            while (offset < st.st_size) {
                sent = sendfile(client_fd, in_fd, &offset, st.st_size - offset);
                if (sent < 0) {
                    syslog(LOG_ERR, "Error %d (%s) on sendfile()", errno, strerror(errno));
                    break;
                }
            }

            close(in_fd);
        }
    }

cleanup:
    if(out_file) {
        fclose(out_file);
    }

    if(caught_signal) {
        syslog(LOG_INFO, "Caught signal, exiting");
        remove(out_filepath);
    }

    if(client_fd != -1) {
        shutdown(client_fd, SHUT_WR);

        syslog(LOG_INFO, "Waiting for FIN from the client...");
        while(read(client_fd, buffer, sizeof(buffer)) > 0) {}
        syslog(LOG_INFO, "Received FIN, closing client socket");

        if(close(client_fd) != 0) {
            syslog(LOG_ERR, "Error %d (%s) on client_fd close()", errno, strerror(errno));
        }
    }

    if(server_fd != -1) {
        if(close(server_fd) != 0) {
            syslog(LOG_ERR, "Error %d (%s) on server_fd close()", errno, strerror(errno));
        }
    }

    if(servinfo) {
        freeaddrinfo(servinfo);
    }

    closelog();
    return ret_code;
}

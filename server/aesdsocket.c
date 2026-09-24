/*
 * aesdsocket - AESD Assignment 5 Part 1
 *
 * Stream socket server on port 9000. Each newline-terminated packet received
 * is appended to DATA_FILE, then the full contents of DATA_FILE are returned
 * to the client. Runs until SIGINT/SIGTERM. Pass -d to run as a daemon.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#define PORT        "9000"
#define DATA_FILE   "/var/tmp/aesdsocketdata"
#define BACKLOG     10
#define CHUNK_SIZE  1024

static volatile sig_atomic_t exit_requested = 0;

static void signal_handler(int signo)
{
    (void)signo;
    exit_requested = 1;
}

/* Install handlers without SA_RESTART so blocking accept()/recv() return EINTR. */
static int setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
        syslog(LOG_ERR, "sigaction failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* Create, configure and bind the listening socket. Returns fd or -1. */
static int open_server_socket(void)
{
    struct addrinfo hints, *res = NULL;
    int sockfd = -1;
    int optval = 1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    rc = getaddrinfo(NULL, PORT, &hints, &res);
    if (rc != 0) {
        syslog(LOG_ERR, "getaddrinfo failed: %s", gai_strerror(rc));
        return -1;
    }

    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd == -1) {
        syslog(LOG_ERR, "socket failed: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) != 0) {
        syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
        close(sockfd);
        freeaddrinfo(res);
        return -1;
    }

    if (bind(sockfd, res->ai_addr, res->ai_addrlen) != 0) {
        syslog(LOG_ERR, "bind failed: %s", strerror(errno));
        close(sockfd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    return sockfd;
}

/* Fork into the background, detach from the terminal, redirect stdio. */
static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid > 0) {
        /* Parent: child now owns the bound socket. */
        exit(EXIT_SUCCESS);
    }

    if (setsid() == -1) {
        syslog(LOG_ERR, "setsid failed: %s", strerror(errno));
        return -1;
    }
    if (chdir("/") != 0) {
        syslog(LOG_ERR, "chdir failed: %s", strerror(errno));
        return -1;
    }

    int devnull = open("/dev/null", O_RDWR);
    if (devnull == -1) {
        syslog(LOG_ERR, "open /dev/null failed: %s", strerror(errno));
        return -1;
    }
    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO)
        close(devnull);

    return 0;
}

/* Send all len bytes, handling partial sends. */
static int send_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t sent = send(fd, buf, len, 0);
        if (sent == -1) {
            if (errno == EINTR)
                continue;
            syslog(LOG_ERR, "send failed: %s", strerror(errno));
            return -1;
        }
        buf += sent;
        len -= (size_t)sent;
    }
    return 0;
}

/* Append one packet to DATA_FILE, then stream the whole file to the client. */
static int handle_packet(int clientfd, const char *packet, size_t len)
{
    int fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd == -1) {
        syslog(LOG_ERR, "open %s for append failed: %s", DATA_FILE, strerror(errno));
        return -1;
    }

    size_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, packet + written, len - written);
        if (w == -1) {
            if (errno == EINTR)
                continue;
            syslog(LOG_ERR, "write failed: %s", strerror(errno));
            close(fd);
            return -1;
        }
        written += (size_t)w;
    }
    close(fd);

    /* Read back in fixed chunks: the file may exceed available RAM. */
    fd = open(DATA_FILE, O_RDONLY);
    if (fd == -1) {
        syslog(LOG_ERR, "open %s for read failed: %s", DATA_FILE, strerror(errno));
        return -1;
    }

    char buf[CHUNK_SIZE];
    ssize_t n;
    int rc = 0;
    while ((n = read(fd, buf, sizeof(buf))) != 0) {
        if (n == -1) {
            if (errno == EINTR)
                continue;
            syslog(LOG_ERR, "read failed: %s", strerror(errno));
            rc = -1;
            break;
        }
        if (send_all(clientfd, buf, (size_t)n) != 0) {
            rc = -1;
            break;
        }
    }
    close(fd);
    return rc;
}

/*
 * Receive from the client until it closes the connection. Data is
 * accumulated in a heap buffer; each newline completes a packet.
 */
static void handle_client(int clientfd)
{
    char *packet = NULL;
    size_t packet_len = 0;
    char recvbuf[CHUNK_SIZE];

    while (true) {
        ssize_t n = recv(clientfd, recvbuf, sizeof(recvbuf), 0);
        if (n == 0)
            break;
        if (n == -1) {
            /* A signal interrupts the recv; finish up with this client. */
            if (errno == EINTR && !exit_requested)
                continue;
            if (errno != EINTR)
                syslog(LOG_ERR, "recv failed: %s", strerror(errno));
            break;
        }

        size_t start = 0;
        for (size_t i = 0; i < (size_t)n; i++) {
            if (recvbuf[i] != '\n')
                continue;

            size_t chunk = i - start + 1;
            char *tmp = realloc(packet, packet_len + chunk);
            if (tmp == NULL) {
                /* Discard the over-length packet and keep going. */
                syslog(LOG_ERR, "realloc failed, discarding packet");
                free(packet);
                packet = NULL;
                packet_len = 0;
            } else {
                packet = tmp;
                memcpy(packet + packet_len, recvbuf + start, chunk);
                packet_len += chunk;
                handle_packet(clientfd, packet, packet_len);
                free(packet);
                packet = NULL;
                packet_len = 0;
            }
            start = i + 1;
        }

        /* Buffer any trailing partial packet. */
        if (start < (size_t)n) {
            size_t chunk = (size_t)n - start;
            char *tmp = realloc(packet, packet_len + chunk);
            if (tmp == NULL) {
                syslog(LOG_ERR, "realloc failed, discarding partial packet");
                free(packet);
                packet = NULL;
                packet_len = 0;
            } else {
                packet = tmp;
                memcpy(packet + packet_len, recvbuf + start, chunk);
                packet_len += chunk;
            }
        }
    }

    free(packet);
}

int main(int argc, char *argv[])
{
    bool daemon_mode = false;
    int opt;

    openlog("aesdsocket", LOG_PID, LOG_USER);

    while ((opt = getopt(argc, argv, "d")) != -1) {
        switch (opt) {
        case 'd':
            daemon_mode = true;
            break;
        default:
            fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
            closelog();
            return -1;
        }
    }

    if (setup_signals() != 0) {
        closelog();
        return -1;
    }

    int sockfd = open_server_socket();
    if (sockfd == -1) {
        closelog();
        return -1;
    }

    /* Fork only after the bind has succeeded. */
    if (daemon_mode && daemonize() != 0) {
        close(sockfd);
        closelog();
        return -1;
    }

    if (listen(sockfd, BACKLOG) != 0) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        close(sockfd);
        closelog();
        return -1;
    }

    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        char ipstr[INET_ADDRSTRLEN];

        int clientfd = accept(sockfd, (struct sockaddr *)&client_addr, &addrlen);
        if (clientfd == -1) {
            if (errno == EINTR)
                continue;
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        inet_ntop(AF_INET, &client_addr.sin_addr, ipstr, sizeof(ipstr));
        syslog(LOG_INFO, "Accepted connection from %s", ipstr);

        handle_client(clientfd);

        close(clientfd);
        syslog(LOG_INFO, "Closed connection from %s", ipstr);
    }

    syslog(LOG_INFO, "Caught signal, exiting");
    close(sockfd);
    if (remove(DATA_FILE) != 0 && errno != ENOENT)
        syslog(LOG_ERR, "remove %s failed: %s", DATA_FILE, strerror(errno));
    closelog();

    return 0;
}

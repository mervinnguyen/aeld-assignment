/*
 * aesdsocket - AESD Assignment 6 Part 1
 *
 * Stream socket server on port 9000. Each connection is handled by its own
 * thread. Each newline-terminated packet received is appended to DATA_FILE,
 * then the full contents of DATA_FILE are returned to the client. A mutex
 * keeps packets from different threads from interleaving. A timer thread
 * appends an RFC 2822 "timestamp:" line every 10 seconds. Runs until
 * SIGINT/SIGTERM, then closes all connections and joins all threads.
 * Pass -d to run as a daemon.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define PORT                "9000"
#define DATA_FILE           "/var/tmp/aesdsocketdata"
#define BACKLOG             10
#define CHUNK_SIZE          1024
#define TIMESTAMP_PERIOD_S  10

static volatile sig_atomic_t exit_requested = 0;

/* Serializes every append + read-back of DATA_FILE. */
static pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Lets the timer thread sleep but wake immediately on shutdown. */
static pthread_mutex_t timer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timer_cond = PTHREAD_COND_INITIALIZER;
static bool timer_stop = false;

struct client_thread {
    pthread_t thread;
    int clientfd;
    char ipstr[INET_ADDRSTRLEN];
    atomic_bool complete;
    SLIST_ENTRY(client_thread) entries;
};

SLIST_HEAD(client_list, client_thread);

static void signal_handler(int signo)
{
    (void)signo;
    exit_requested = 1;
}

/* Install handlers without SA_RESTART so blocking accept() returns EINTR. */
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

/* Block or unblock SIGINT/SIGTERM for the calling thread. */
static void set_exit_signals_blocked(bool blocked)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(blocked ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL);
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
        ssize_t sent = send(fd, buf, len, MSG_NOSIGNAL);
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

/* Append len bytes to DATA_FILE. Caller must hold data_mutex. */
static int append_data(const char *data, size_t len)
{
    int fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd == -1) {
        syslog(LOG_ERR, "open %s for append failed: %s", DATA_FILE, strerror(errno));
        return -1;
    }

    size_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, data + written, len - written);
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
    return 0;
}

/* Append one packet to DATA_FILE, then stream the whole file to the client. */
static int handle_packet(int clientfd, const char *packet, size_t len)
{
    int rc = 0;

    pthread_mutex_lock(&data_mutex);

    if (append_data(packet, len) != 0) {
        pthread_mutex_unlock(&data_mutex);
        return -1;
    }

    /* Read back in fixed chunks: the file may exceed available RAM. */
    int fd = open(DATA_FILE, O_RDONLY);
    if (fd == -1) {
        syslog(LOG_ERR, "open %s for read failed: %s", DATA_FILE, strerror(errno));
        pthread_mutex_unlock(&data_mutex);
        return -1;
    }

    char buf[CHUNK_SIZE];
    ssize_t n;
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

    pthread_mutex_unlock(&data_mutex);
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
            if (errno == EINTR)
                continue;
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

static void *client_thread_fn(void *arg)
{
    struct client_thread *ct = arg;

    handle_client(ct->clientfd);
    syslog(LOG_INFO, "Closed connection from %s", ct->ipstr);

    /* main joins the thread and closes clientfd once it sees this flag. */
    ct->complete = true;
    return NULL;
}

/* Every TIMESTAMP_PERIOD_S seconds, append "timestamp:<RFC 2822 time>\n". */
static void *timer_thread_fn(void *arg)
{
    (void)arg;
    struct timespec next;
    clock_gettime(CLOCK_REALTIME, &next);

    pthread_mutex_lock(&timer_mutex);
    while (!timer_stop) {
        next.tv_sec += TIMESTAMP_PERIOD_S;
        int rc;
        do {
            rc = pthread_cond_timedwait(&timer_cond, &timer_mutex, &next);
        } while (rc != ETIMEDOUT && !timer_stop);
        if (timer_stop)
            break;

        char line[128];
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        size_t len = strftime(line, sizeof(line), "timestamp:%a, %d %b %Y %H:%M:%S %z\n", &tm_now);
        if (len == 0) {
            syslog(LOG_ERR, "strftime failed");
            continue;
        }

        pthread_mutex_lock(&data_mutex);
        append_data(line, len);
        pthread_mutex_unlock(&data_mutex);
    }
    pthread_mutex_unlock(&timer_mutex);
    return NULL;
}

/* Join and free every client thread that has finished (or all, if force). */
static void reap_client_threads(struct client_list *head, bool force)
{
    struct client_thread *ct = SLIST_FIRST(head);
    struct client_thread *prev = NULL;

    while (ct != NULL) {
        struct client_thread *next = SLIST_NEXT(ct, entries);
        if (force || ct->complete) {
            if (force && !ct->complete)
                shutdown(ct->clientfd, SHUT_RDWR);   /* unblock recv() */
            pthread_join(ct->thread, NULL);
            close(ct->clientfd);
            if (prev == NULL)
                SLIST_REMOVE_HEAD(head, entries);
            else
                SLIST_NEXT(prev, entries) = next;
            free(ct);
        } else {
            prev = ct;
        }
        ct = next;
    }
}

int main(int argc, char *argv[])
{
    bool daemon_mode = false;
    int opt;
    int rc = 0;

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

    /* Fork only after the bind has succeeded, and before creating threads. */
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

    /* Worker threads inherit a blocked mask, so signals land on main's accept(). */
    set_exit_signals_blocked(true);

    pthread_t timer_thread;
    if (pthread_create(&timer_thread, NULL, timer_thread_fn, NULL) != 0) {
        syslog(LOG_ERR, "pthread_create for timer failed");
        close(sockfd);
        closelog();
        return -1;
    }

    struct client_list clients;
    SLIST_INIT(&clients);

    set_exit_signals_blocked(false);

    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);

        int clientfd = accept(sockfd, (struct sockaddr *)&client_addr, &addrlen);
        if (clientfd == -1) {
            if (errno != EINTR)
                syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        struct client_thread *ct = calloc(1, sizeof(*ct));
        if (ct == NULL) {
            syslog(LOG_ERR, "calloc failed, dropping connection");
            close(clientfd);
            continue;
        }
        ct->clientfd = clientfd;
        inet_ntop(AF_INET, &client_addr.sin_addr, ct->ipstr, sizeof(ct->ipstr));
        syslog(LOG_INFO, "Accepted connection from %s", ct->ipstr);

        set_exit_signals_blocked(true);
        int prc = pthread_create(&ct->thread, NULL, client_thread_fn, ct);
        set_exit_signals_blocked(false);
        if (prc != 0) {
            syslog(LOG_ERR, "pthread_create failed: %s", strerror(prc));
            close(clientfd);
            free(ct);
            continue;
        }
        SLIST_INSERT_HEAD(&clients, ct, entries);

        reap_client_threads(&clients, false);
    }

    syslog(LOG_INFO, "Caught signal, exiting");

    close(sockfd);
    reap_client_threads(&clients, true);

    pthread_mutex_lock(&timer_mutex);
    timer_stop = true;
    pthread_cond_signal(&timer_cond);
    pthread_mutex_unlock(&timer_mutex);
    pthread_join(timer_thread, NULL);

    if (remove(DATA_FILE) != 0 && errno != ENOENT) {
        syslog(LOG_ERR, "remove %s failed: %s", DATA_FILE, strerror(errno));
        rc = -1;
    }
    closelog();

    return rc;
}

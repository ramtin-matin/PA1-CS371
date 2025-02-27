/*
# Copyright 2025 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# SPDX-License-Identifier: Apache-2.0
#
# Group Members:
# - David Falade
# - Parker Nurick
# - Ramtin Seyedmatin
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4
#define DEFAULT_SERVER_THREADS 4

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

/* ---------------- CLIENT CODE ---------------- */

typedef struct {
    int epoll_fd;
    int socket_fd;
    long long total_rtt;
    long total_messages;
    float request_rate;
} client_thread_data_t;

void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    char send_buf[MESSAGE_SIZE + 1] = "ABCDEFGHIJKMLNOP";
    char recv_buf[MESSAGE_SIZE + 1];
    struct timeval start, end;

    send_buf[MESSAGE_SIZE] = '\0';

    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) == -1) {
        perror("epoll_ctl (client)");
        pthread_exit(NULL);
    }

    for (long i = 0; i < num_requests; i++) {
        gettimeofday(&start, NULL);
        if (send(data->socket_fd, send_buf, MESSAGE_SIZE, 0) == -1) {
            perror("send");
            continue;
        }
        int nfds = epoll_wait(data->epoll_fd, events, MAX_EVENTS, 2000);
        if (nfds == -1) {
            perror("epoll_wait");
            continue;
        }
        for (int j = 0; j < nfds; j++) {
            if (events[j].data.fd == data->socket_fd) {
                int bytes_received = recv(data->socket_fd, recv_buf, MESSAGE_SIZE, 0);
                if (bytes_received > 0) {
                    recv_buf[bytes_received] = '\0';
                    gettimeofday(&end, NULL);
                    long long rtt = (end.tv_sec - start.tv_sec) * 1000000LL +
                                      (end.tv_usec - start.tv_usec);
                    data->total_rtt += rtt;
                    data->total_messages++;
                }
            }
        }
    }
    data->request_rate = (data->total_messages > 0) ? 
                         (data->total_messages / (data->total_rtt / 1000000.0)) : 0;
    close(data->socket_fd);
    close(data->epoll_fd);
    return NULL;
}

void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    long long total_rtt = 0;
    long total_messages = 0;
    float total_request_rate = 0.0;

    for (int i = 0; i < num_client_threads; i++) {
        thread_data[i].socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        thread_data[i].epoll_fd = epoll_create1(0);
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(server_port);
        inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

        if (connect(thread_data[i].socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
            perror("connect");
            exit(EXIT_FAILURE);
        }
        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
        total_rtt += thread_data[i].total_rtt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;
    }
    printf("Total Messages: %ld, Total RTT: %lld us\n", total_messages, total_rtt);
    printf("Average RTT: %lld us\n", (total_messages > 0) ? (total_rtt / total_messages) : 0);
    printf("Total Request Rate: %f messages/s\n", total_request_rate);
}

/* ---------------- MULTI-THREADED SERVER (Bonus Implementation) ---------------- */

typedef struct {
    int epoll_fd;      // epoll instance for the worker
    int notify_fd;     // read end of the notification pipe
} worker_arg_t;

#define FD_BUF_SIZE 10

// Worker thread function for handling client sockets
void *worker_thread_func(void *arg) {
    worker_arg_t *warg = (worker_arg_t *)arg;
    int epoll_fd = warg->epoll_fd;
    struct epoll_event events[MAX_EVENTS];
    char fd_buf[FD_BUF_SIZE];
    while (1) {
        int n = read(warg->notify_fd, fd_buf, FD_BUF_SIZE);
        if (n > 0) {
            int client_fd;
            memcpy(&client_fd, fd_buf, sizeof(int));
            // Add the client fd to epoll for this worker
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.fd = client_fd;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                perror("epoll_ctl worker add client");
                close(client_fd);
            }
        }
        
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
        for (int i = 0; i < nfds; i++) {
            int client_fd = events[i].data.fd;
            char buf[MESSAGE_SIZE];
            int bytes_received = recv(client_fd, buf, MESSAGE_SIZE, 0);
            if (bytes_received > 0) {
                send(client_fd, buf, bytes_received, 0);
            } else if (bytes_received == 0) {
                close(client_fd);
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("recv worker");
                    close(client_fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                }
            }
        }
    }
    return NULL;
}

void run_server_mt() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) { perror("socket"); exit(EXIT_FAILURE); }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind"); exit(EXIT_FAILURE);
    }
    if (listen(server_fd, SOMAXCONN) == -1) { perror("listen"); exit(EXIT_FAILURE); }

    // Create worker threads
    int num_workers = DEFAULT_SERVER_THREADS;
    pthread_t workers[num_workers];
    worker_arg_t worker_args[num_workers];
    // We create a pipe per worker for notifications.
    int notify_pipes[num_workers][2];

    for (int i = 0; i < num_workers; i++) {
        if (pipe(notify_pipes[i]) == -1) {
            perror("pipe"); exit(EXIT_FAILURE);
        }
        worker_args[i].epoll_fd = epoll_create1(0);
        if (worker_args[i].epoll_fd == -1) {
            perror("epoll_create1 worker"); exit(EXIT_FAILURE);
        }
        worker_args[i].notify_fd = notify_pipes[i][0];

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = worker_args[i].notify_fd;
        if (epoll_ctl(worker_args[i].epoll_fd, EPOLL_CTL_ADD, worker_args[i].notify_fd, &ev) == -1) {
            perror("epoll_ctl add notify_fd"); exit(EXIT_FAILURE);
        }

        pthread_create(&workers[i], NULL, worker_thread_func, &worker_args[i]);
    }

    int next_worker = 0;
    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            perror("accept");
            continue;
        }
        if (write(notify_pipes[next_worker][1], &client_fd, sizeof(client_fd)) != sizeof(client_fd)) {
            perror("write to pipe");
            close(client_fd);
        }
        next_worker = (next_worker + 1) % num_workers;
    }
}

/* ---------------- MAIN ---------------- */

int main(int argc, char *argv[]) {
    if (argc >= 2) {
        if (strcmp(argv[1], "server") == 0) {
            if (argc > 2) server_ip = argv[2];
            if (argc > 3) server_port = atoi(argv[3]);
            // Run single-threaded server
            run_server();
        } else if (strcmp(argv[1], "server_mt") == 0) {
            if (argc > 2) server_ip = argv[2];
            if (argc > 3) server_port = atoi(argv[3]);
            // Run multi-threaded (bonus) server
            run_server_mt();
        } else if (strcmp(argv[1], "client") == 0) {
            if (argc > 2) server_ip = argv[2];
            if (argc > 3) server_port = atoi(argv[3]);
            if (argc > 4) num_client_threads = atoi(argv[4]);
            if (argc > 5) num_requests = atoi(argv[5]);
            run_client();
        } else {
            printf("Usage: %s <server|server_mt|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
        }
    } else {
        printf("Usage: %s <server|server_mt|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
    }
    return 0;
}

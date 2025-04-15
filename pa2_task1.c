/********************************************************
 * pa2_task1.c
 *
 * CS371 PA2 - Task 1
 *   UDP-based "Stop-and-Wait" Protocol
 *
 * Build Command (example):
 *   gcc -o pa2_task1 pa2_task1.c -pthread
 *
 * Usage (example):
 *   ./pa2_task1 server 127.0.0.1 5000 4 1000
 *   ./pa2_task1 client 127.0.0.1 5000 4 1000
 *
 * ------------------------------------------------------
 * Satisfies requirements:
 *  1) Change to UDP socket
 *  2) Implement metrics collection (tx_cnt, rx_cnt)
 *  3) Observe packet loss with many client threads
 *
 ********************************************************/

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
 
 // For clarity; you can tune these
 #define MAX_EVENTS      64
 #define MESSAGE_SIZE    16
 #define DEFAULT_THREADS 4
 
 // Global command-line args
 static char *server_ip          = NULL;
 static int   server_port        = 0;
 static int   num_client_threads = 0;
 static int   num_requests       = 0;
 
 /*
  * Per-thread data for client
  */
 typedef struct {
     int epoll_fd;           // epoll instance
     int sock_fd;            // UDP socket FD
     long long total_rtt;    // aggregated RTT (microseconds)
     long total_messages;    // total successfully received messages
     long tx_count;          // total transmissions
     long rx_count;          // total receives
 } client_thread_data_t;
 
 /*
  * Thread function for each client "Stop-and-Wait" approach
  */
 void* client_thread_func(void *arg)
 {
     client_thread_data_t *data = (client_thread_data_t*)arg;
     char send_buf[MESSAGE_SIZE] = "ABCDEFGHIJKLNMOP"; // 16B
     char recv_buf[MESSAGE_SIZE];
     struct epoll_event ev, events[MAX_EVENTS];
 
     // Register the socket with epoll for read events
     memset(&ev, 0, sizeof(ev));
     ev.events  = EPOLLIN;
     ev.data.fd = data->sock_fd;
     if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->sock_fd, &ev) < 0) {
         perror("epoll_ctl ADD client socket");
         pthread_exit(NULL);
     }
 
     for (int i = 0; i < num_requests; i++) {
         struct timeval start, end;
 
         // Record time before send
         gettimeofday(&start, NULL);
 
         // Send a single packet (Stop-and-Wait means 1 packet, then wait)
         ssize_t sent = send(data->sock_fd, send_buf, MESSAGE_SIZE, 0);
         if (sent != MESSAGE_SIZE) {
             perror("send() failed");
             break;
         }
         data->tx_count++;
 
         // Wait (blocking) for a read event from epoll
         int nfds = epoll_wait(data->epoll_fd, events, MAX_EVENTS, -1);
         if (nfds < 0) {
             perror("epoll_wait error");
             break;
         }
 
         // Read the response
         for (int e = 0; e < nfds; e++) {
             if (events[e].data.fd == data->sock_fd) {
                 ssize_t recvd = recv(data->sock_fd, recv_buf, MESSAGE_SIZE, 0);
                 if (recvd == MESSAGE_SIZE) {
                     data->rx_count++;
                     // measure RTT
                     gettimeofday(&end, NULL);
                     long long start_us = (long long)start.tv_sec * 1000000 + start.tv_usec;
                     long long end_us   = (long long)end.tv_sec   * 1000000 + end.tv_usec;
                     data->total_rtt   += (end_us - start_us);
                     data->total_messages++;
                 }
             }
         }
     }
 
     pthread_exit(NULL);
 }
 
 /*
  * Launch multiple client threads that each run a UDP "Stop-and-Wait" flow,
  * measure total packet loss and average RTT.
  */
 void run_client()
 {
     pthread_t *threads = calloc(num_client_threads, sizeof(pthread_t));
     client_thread_data_t *tdata = calloc(num_client_threads, sizeof(client_thread_data_t));
 
     // Create each client's socket, epoll, etc.
     for (int i = 0; i < num_client_threads; i++) {
         // Create UDP socket
         int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
         if (sockfd < 0) {
             perror("socket() client");
             continue;
         }
 
         // "Connect" for convenience (so we can use send/recv instead of sendto/recvfrom)
         struct sockaddr_in servaddr;
         memset(&servaddr, 0, sizeof(servaddr));
         servaddr.sin_family = AF_INET;
         servaddr.sin_port   = htons(server_port);
         if (inet_pton(AF_INET, server_ip, &servaddr.sin_addr) <= 0) {
             perror("inet_pton failed (client)");
             close(sockfd);
             continue;
         }
 
         if (connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
             perror("connect() failed (client)");
             close(sockfd);
             continue;
         }
 
         // Create epoll instance
         int efd = epoll_create1(0);
         if (efd < 0) {
             perror("epoll_create1 failed (client)");
             close(sockfd);
             continue;
         }
 
         // Fill in thread data
         tdata[i].sock_fd       = sockfd;
         tdata[i].epoll_fd      = efd;
         tdata[i].total_rtt     = 0;
         tdata[i].total_messages= 0;
         tdata[i].tx_count      = 0;
         tdata[i].rx_count      = 0;
 
         pthread_create(&threads[i], NULL, client_thread_func, &tdata[i]);
     }
 
     // Wait for all threads
     long long grand_total_rtt = 0;
     long long grand_total_msgs = 0;
     long grand_tx = 0, grand_rx = 0;
 
     for (int i = 0; i < num_client_threads; i++) {
         pthread_join(threads[i], NULL);
 
         grand_total_rtt  += tdata[i].total_rtt;
         grand_total_msgs += tdata[i].total_messages;
         grand_tx         += tdata[i].tx_count;
         grand_rx         += tdata[i].rx_count;
 
         // clean up
         close(tdata[i].sock_fd);
         close(tdata[i].epoll_fd);
     }
 
     free(threads);
     free(tdata);
 
     // Final metrics
     printf("[Client Summary]\n");
     printf("  Total transmitted = %ld\n", grand_tx);
     printf("  Total received    = %ld\n", grand_rx);
     printf("  Lost packets      = %ld\n", (grand_tx - grand_rx));
 
     if (grand_total_msgs > 0) {
         long long avg_rtt = grand_total_rtt / grand_total_msgs;
         double total_sec  = (double)grand_total_rtt / 1000000.0;
         double req_rate   = (double)grand_total_msgs / total_sec;  // requests/second
         printf("  Average RTT       = %lld us\n", avg_rtt);
         printf("  Aggregate ReqRate = %.2f req/s\n", req_rate);
     } else {
         printf("  No successful messages.\n");
     }
 }
 
 /*
  * UDP server that echoes back any datagram it receives.
  * We'll use epoll to monitor the single UDP socket for inbound data.
  */
 void run_server()
 {
     int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
     if (server_fd < 0) {
         perror("socket() server");
         exit(1);
     }
 
     // Bind
     struct sockaddr_in srv_addr;
     memset(&srv_addr, 0, sizeof(srv_addr));
     srv_addr.sin_family      = AF_INET;
     srv_addr.sin_port        = htons(server_port);
     if (inet_pton(AF_INET, server_ip, &srv_addr.sin_addr) <= 0) {
         perror("inet_pton (server)");
         close(server_fd);
         exit(1);
     }
 
     if (bind(server_fd, (struct sockaddr*)&srv_addr, sizeof(srv_addr)) < 0) {
         perror("bind() failed (server)");
         close(server_fd);
         exit(1);
     }
 
     // Create epoll
     int epoll_fd = epoll_create1(0);
     if (epoll_fd < 0) {
         perror("epoll_create1 (server)");
         close(server_fd);
         exit(1);
     }
 
     struct epoll_event ev, events[MAX_EVENTS];
     memset(&ev, 0, sizeof(ev));
     ev.events  = EPOLLIN;
     ev.data.fd = server_fd;
     if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
         perror("epoll_ctl ADD (server)");
         close(server_fd);
         close(epoll_fd);
         exit(1);
     }
 
     printf("UDP Server listening on %s:%d ...\n", server_ip, server_port);
 
     // Server main loop
     while (1) {
         int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
         if (nfds < 0) {
             perror("epoll_wait (server)");
             break;
         }
 
         for (int i = 0; i < nfds; i++) {
             if (events[i].data.fd == server_fd) {
                 // We have data waiting on our UDP socket
                 while (1) {
                     struct sockaddr_in client_addr;
                     socklen_t client_len = sizeof(client_addr);
                     char buffer[MESSAGE_SIZE];
 
                     ssize_t recvd = recvfrom(server_fd, buffer, MESSAGE_SIZE, MSG_DONTWAIT,
                                              (struct sockaddr*)&client_addr, &client_len);
                     if (recvd <= 0) {
                         // Either no more packets or error
                         break;
                     }
 
                     // Echo back
                     sendto(server_fd, buffer, recvd, 0,
                            (struct sockaddr*)&client_addr, client_len);
                 }
             }
         }
     }
 
     // Cleanup
     close(server_fd);
     close(epoll_fd);
 }
 
 /*
  * Main entrypoint.  Usage:
  *  ./pa2_task1 <server|client> <server_ip> <server_port> <num_threads> <num_requests>
  */
 int main(int argc, char *argv[])
 {
     if (argc != 6) {
         fprintf(stderr, "Usage: %s <server|client> <server_ip> <server_port> <num_client_threads> <num_requests>\n", argv[0]);
         exit(1);
     }
 
     // Parse
     char *mode = argv[1];
     server_ip  = argv[2];
     server_port= atoi(argv[3]);
     num_client_threads = atoi(argv[4]);
     num_requests       = atoi(argv[5]);
 
     if (strcmp(mode, "server") == 0) {
         run_server();
     } else if (strcmp(mode, "client") == 0) {
         run_client();
     } else {
         fprintf(stderr, "Invalid mode: %s (must be server or client)\n", mode);
         exit(1);
     }
 
     return 0;
 }
 
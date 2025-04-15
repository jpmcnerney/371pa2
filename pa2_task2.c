/********************************************************
 * pa2_task2.c
 *
 * CS371 PA2 - Task 2
 *   UDP-based SN + ARQ
 *
 * Build Command (example):
 *   gcc -o pa2_task2 pa2_task2.c -pthread
 *
 * Usage (example):
 *   ./pa2_task2 server 127.0.0.1 5000 4 1000
 *   ./pa2_task2 client 127.0.0.1 5000 4 1000
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
 
 #define MAX_EVENTS      64
 #define MESSAGE_SIZE    16
 #define DEFAULT_THREADS 4
 #define EPOLL_TIMEOUT_MS 100    // epoll timeout (for ARQ retransmission)
 
 // Global command-line args
 static char *server_ip          = NULL;
 static int   server_port        = 0;
 static int   num_client_threads = 0;
 static int   num_requests       = 0;
 
 /*
  * Example packet structure to carry:
  *   - client_id
  *   - seq_num
  *   - data (16 bytes)
  */
 typedef struct {
     int  client_id;
     int  seq_num;
     char data[MESSAGE_SIZE];
 } mypacket_t;
 
 /*
  * Per-thread data for client
  */
 typedef struct {
     int epoll_fd;           // epoll instance
     int sock_fd;            // UDP socket FD
     int thread_id;          // which client thread is this?
     long long total_rtt;    // aggregated RTT (microseconds)
     long total_messages;    // total messages for which we got ack
     long tx_count;          // total transmissions (not counting retrans)
     long rx_count;          // total acks
 } client_thread_data_t;
 
 /*
  * ARQ client thread function
  */
 void* client_thread_func(void *arg)
 {
     client_thread_data_t *data = (client_thread_data_t*)arg;
     struct epoll_event ev, events[MAX_EVENTS];
 
     // Register the socket with epoll
     memset(&ev, 0, sizeof(ev));
     ev.events  = EPOLLIN;
     ev.data.fd = data->sock_fd;
     if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->sock_fd, &ev) < 0) {
         perror("epoll_ctl ADD client socket");
         pthread_exit(NULL);
     }
 
     // We'll do a simple Stop-and-Wait with sequence numbers
     for (int seq = 0; seq < num_requests; seq++) {
         mypacket_t packet;
         memset(&packet, 0, sizeof(packet));
         packet.client_id = data->thread_id;
         packet.seq_num   = seq;
         memcpy(packet.data, "HELLO_012345678", MESSAGE_SIZE);
 
         // We'll keep sending until we get the correct ack
         struct timeval start;
         gettimeofday(&start, NULL);
 
         // Since instructions say: "When retransmitting a packet, do NOT increment tx_cnt",
         // we store it in a variable outside the loop:
         data->tx_count++;
 
         int got_ack = 0;
         while (!got_ack) {
             // Send packet
             ssize_t sent = send(data->sock_fd, &packet, sizeof(packet), 0);
             if (sent != sizeof(packet)) {
                 perror("send() failed");
                 break;
             }
 
             // Wait up to EPOLL_TIMEOUT_MS for ack
             int nfds = epoll_wait(data->epoll_fd, events, MAX_EVENTS, EPOLL_TIMEOUT_MS);
             if (nfds < 0) {
                 perror("epoll_wait error");
                 break;
             } else if (nfds == 0) {
                 // Timeout -> retransmit the same packet (do NOT increment tx_count)
                 continue; 
             }
 
             // If we got something, let's read it:
             for (int e = 0; e < nfds; e++) {
                 if (events[e].data.fd == data->sock_fd) {
                     mypacket_t ack;
                     ssize_t recvd = recv(data->sock_fd, &ack, sizeof(ack), 0);
                     if (recvd == sizeof(ack)) {
                         // Check if it is indeed the ack for our current (client_id, seq_num)
                         if (ack.client_id == data->thread_id && ack.seq_num == seq) {
                             // Great, this is our ack
                             data->rx_count++;
                             got_ack = 1;
 
                             // measure RTT
                             struct timeval end;
                             gettimeofday(&end, NULL);
                             long long start_us = (long long)start.tv_sec * 1000000 + start.tv_usec;
                             long long end_us   = (long long)end.tv_sec   * 1000000 + end.tv_usec;
                             data->total_rtt   += (end_us - start_us);
                             data->total_messages++;
                             break;
                         }
                     }
                 }
             }
         }
     }
 
     pthread_exit(NULL);
 }
 
 /*
  * Launch multiple ARQ client threads, measure final metrics.
  */
 void run_client()
 {
     pthread_t *threads = calloc(num_client_threads, sizeof(pthread_t));
     client_thread_data_t *tdata = calloc(num_client_threads, sizeof(client_thread_data_t));
 
     // Create each client's socket, epoll, etc.
     for (int i = 0; i < num_client_threads; i++) {
         int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
         if (sockfd < 0) {
             perror("socket() client");
             continue;
         }
 
         // Connect
         struct sockaddr_in servaddr;
         memset(&servaddr, 0, sizeof(servaddr));
         servaddr.sin_family = AF_INET;
         servaddr.sin_port   = htons(server_port);
         if (inet_pton(AF_INET, server_ip, &servaddr.sin_addr) <= 0) {
             perror("inet_pton() failed (client)");
             close(sockfd);
             continue;
         }
 
         if (connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
             perror("connect() failed (client)");
             close(sockfd);
             continue;
         }
 
         // Create epoll
         int efd = epoll_create1(0);
         if (efd < 0) {
             perror("epoll_create1() failed (client)");
             close(sockfd);
             continue;
         }
 
         tdata[i].sock_fd        = sockfd;
         tdata[i].epoll_fd       = efd;
         tdata[i].thread_id      = i;
         tdata[i].total_rtt      = 0;
         tdata[i].total_messages = 0;
         tdata[i].tx_count       = 0;
         tdata[i].rx_count       = 0;
 
         pthread_create(&threads[i], NULL, client_thread_func, &tdata[i]);
     }
 
     // Wait and collect stats
     long long grand_total_rtt  = 0;
     long long grand_total_msgs = 0;
     long grand_tx = 0, grand_rx = 0;
 
     for (int i = 0; i < num_client_threads; i++) {
         pthread_join(threads[i], NULL);
 
         grand_total_rtt  += tdata[i].total_rtt;
         grand_total_msgs += tdata[i].total_messages;
         grand_tx         += tdata[i].tx_count;
         grand_rx         += tdata[i].rx_count;
 
         close(tdata[i].sock_fd);
         close(tdata[i].epoll_fd);
     }
 
     free(threads);
     free(tdata);
 
     // Summaries
     printf("[Client Summary - ARQ]\n");
     printf("  Total transmitted (first-send only) = %ld\n", grand_tx);
     printf("  Total acked                         = %ld\n", grand_rx);
     printf("  Lost packets (should be 0)          = %ld\n", (grand_tx - grand_rx));
 
     if (grand_total_msgs > 0) {
         long long avg_rtt = grand_total_rtt / grand_total_msgs;
         double total_sec  = (double)grand_total_rtt / 1000000.0;
         double req_rate   = (double)grand_total_msgs / total_sec;
         printf("  Average RTT       = %lld us\n", avg_rtt);
         printf("  Aggregate ReqRate = %.2f req/s\n", req_rate);
     } else {
         printf("  No successful messages.\n");
     }
 }
 
 /*
  * Server side: single UDP socket, listens for packets, and simply echoes them back.
  * We can just echo the entire struct (client_id, seq_num, data).
  */
 void run_server()
 {
     int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
     if (server_fd < 0) {
         perror("socket() server");
         exit(1);
     }
 
     struct sockaddr_in srv_addr;
     memset(&srv_addr, 0, sizeof(srv_addr));
     srv_addr.sin_family = AF_INET;
     srv_addr.sin_port   = htons(server_port);
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
 
     // Setup epoll
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
 
     printf("UDP Server (SN+ARQ) listening on %s:%d ...\n", server_ip, server_port);
 
     // Event loop
     while (1) {
         int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
         if (nfds < 0) {
             perror("epoll_wait (server)");
             break;
         }
 
         for (int i = 0; i < nfds; i++) {
             if (events[i].data.fd == server_fd) {
                 // read all available packets
                 while (1) {
                     struct sockaddr_in client_addr;
                     socklen_t client_len = sizeof(client_addr);
                     mypacket_t pkt;
 
                     ssize_t recvd = recvfrom(server_fd, &pkt, sizeof(pkt), MSG_DONTWAIT,
                                              (struct sockaddr*)&client_addr, &client_len);
                     if (recvd <= 0) {
                         // either no more data or an error
                         break;
                     }
 
                     // echo
                     sendto(server_fd, &pkt, recvd, 0,
                            (struct sockaddr*)&client_addr, client_len);
                 }
             }
         }
     }
 
     close(server_fd);
     close(epoll_fd);
 }
 
 /*
  * Main entrypoint
  */
 int main(int argc, char *argv[])
 {
     if (argc != 6) {
         fprintf(stderr, "Usage: %s <server|client> <server_ip> <server_port> <num_client_threads> <num_requests>\n", argv[0]);
         exit(1);
     }
 
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
         fprintf(stderr, "Invalid mode: %s\n", mode);
         exit(1);
     }
 
     return 0;
 }
 
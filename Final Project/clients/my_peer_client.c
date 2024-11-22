#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>

#define MAX_BUFFER_SIZE 1024
#define MAX_CONTENT_SIZE 100
#define MAX_FILENAME_SIZE 256
#define DEFAULT_PORT 3000
#define CHUNK_SIZE 1024
#define TRANSFER_TIMEOUT 3
#define MAX_RETRIES 8

/* PDU Types */
#define PDU_TYPE_R 'R'  // Registration
#define PDU_TYPE_D 'D'  // Download
#define PDU_TYPE_S 'S'  // Search
#define PDU_TYPE_T 'T'  // De-registration
#define PDU_TYPE_C 'C'  // Content
#define PDU_TYPE_O 'O'  // List
#define PDU_TYPE_A 'A'  // Ack
#define PDU_TYPE_E 'E'  // Error

/* PDU Structures */
struct __attribute__((__packed__)) pdu_register {
    char type;
    uint8_t pNameLength;
    char pName[MAX_CONTENT_SIZE];
    uint8_t cNameLength;
    char cName[MAX_CONTENT_SIZE];
    uint32_t address;
    uint16_t port;
};

struct __attribute__((__packed__)) pdu_download {
    char type;
    uint8_t cNameLength;
    char cName[MAX_CONTENT_SIZE];
};

struct __attribute__((__packed__)) pdu_search {
    char type;
    uint8_t pNameLength;
    char pName[MAX_CONTENT_SIZE];
    uint8_t cNameLength;
    char cName[MAX_CONTENT_SIZE];
};

struct __attribute__((__packed__)) pdu_content {
    char type;
    uint32_t length;
    char data[CHUNK_SIZE];
};

/* Content Host Management */
struct content_host {
    char pName[MAX_CONTENT_SIZE];
    char cName[MAX_CONTENT_SIZE];
    uint32_t address;
    uint16_t port;
    struct content_host* next;
};

/* Global State */
static struct {
    int peer_id;
    int udp_sock;
    struct sockaddr_in server_addr;
    struct content_host* hosted_content;
    uint16_t next_tcp_port;
} peer_state;

/* Function Prototypes */
static void init_peer(int peer_id, const char* host, int port);
static void handle_user_input(void);
static int register_content(const char* filename);
static int download_content(const char* filename);
static int list_content(void);
static int deregister_content(const char* filename);
static void cleanup_and_exit(void);
static void handle_child_exit(int sig);

/* TCP Server Functions */
static int start_tcp_server(const char* filename);
static void handle_download_request(int client_sock, const char* filename);
static int send_file(int sock, const char* filename);
static int receive_file(int sock, const char* filename);

/* Helper Functions */
static struct content_host* find_content(const char* filename);
static void add_content(const char* filename, uint32_t addr, uint16_t port);
static void remove_content(const char* filename);
static void set_socket_timeout(int sock, int seconds);

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <peer_id> [host] [port]\n"
                "       peer_id: Unique identifier for this peer\n"
                "       host: Hostname or IP address of the index server (default: localhost)\n"
                "       port: Port number of the index server (default: %d)\n", argv[0], DEFAULT_PORT);
        exit(1);
    }

    int peer_id = atoi(argv[1]);
    const char* host = (argc > 2) ? argv[2] : "localhost";
    int port = (argc > 3) ? atoi(argv[3]) : DEFAULT_PORT;

    signal(SIGCHLD, handle_child_exit);
    init_peer(peer_id, host, port);
    handle_user_input();

    return 0;
}

static void init_peer(int peer_id, const char* host, int port) {
    peer_state.peer_id = peer_id;
    peer_state.hosted_content = NULL;
    peer_state.next_tcp_port = port + 1000 + (peer_id * 100);

    // Setup UDP socket for index server communication
    peer_state.udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (peer_state.udp_sock < 0) {
        perror("Failed to create UDP socket");
        exit(1);
    }

    int reuse = 1;
    //This code allows the socket to be reused immediately after the program exits. 
    if (setsockopt(peer_state.udp_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("Failed to set socket reuse option");
        exit(1);
    }

    // Configure server address
    memset(&peer_state.server_addr, 0, sizeof(peer_state.server_addr));
    peer_state.server_addr.sin_family = AF_INET;
    peer_state.server_addr.sin_port = htons(port);

    struct hostent* he = gethostbyname(host);
    if (!he) {
        fprintf(stderr, "Failed to resolve host: %s\n", host);
        exit(1);
    }
    memcpy(&peer_state.server_addr.sin_addr, he->h_addr_list[0], he->h_length);

    set_socket_timeout(peer_state.udp_sock, TRANSFER_TIMEOUT);
}

static void handle_user_input(void) {
    char command[MAX_BUFFER_SIZE];
    char filename[MAX_FILENAME_SIZE];

    while (1) {
        printf("\nAvailable commands:\n");
        printf("R - Register content\n");
        printf("D - Download content\n");
        printf("O - List content\n");
        printf("T - Deregister content\n");
        printf("Q - Quit\n");
        printf("\nEnter command: ");

        if (fgets(command, sizeof(command), stdin) == NULL) {
            break;
        }

        switch (command[0]) {
            case 'R':
            case 'r':
                printf("Enter filename to register: ");
                if (fgets(filename, sizeof(filename), stdin)) {
                    filename[strcspn(filename, "\n")] = 0;
                    register_content(filename);
                }
                break;

            case 'D':
            case 'd':
                printf("Enter filename to download: ");
                if (fgets(filename, sizeof(filename), stdin)) {
                    filename[strcspn(filename, "\n")] = 0;
                    download_content(filename);
                }
                break;

            case 'O':
            case 'o':
                list_content();
                break;

            case 'T':
            case 't':
                printf("Enter filename to deregister: ");
                if (fgets(filename, sizeof(filename), stdin)) {
                    filename[strcspn(filename, "\n")] = 0;
                    deregister_content(filename);
                }
                break;

            case 'Q':
            case 'q':
                cleanup_and_exit();
                break;

            default:
                printf("Unknown command\n");
        }
    }
}

static int register_content(const char* filename) {
    // Check if file exists and is readable
    FILE* f = fopen(filename, "r");
    if (!f) {
        perror("Failed to open file");
        return -1;
    }
    fclose(f);

    // Check if content is already registered
    struct content_host* existing = find_content(filename);
    if (existing) {
        printf("Content '%s' is already registered\n", filename);
        return -1;
    }

    // Start TCP server for this content
    int tcp_port = start_tcp_server(filename);
    if (tcp_port < 0) {
        return -1;
    }

    // Get local IP address that can reach the server
    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    
    // Create a temporary UDP socket to determine the correct local address
    int temp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (temp_sock < 0) {
        perror("Failed to create temporary socket");
        return -1;
    }
    
    // Connect UDP socket to server address (doesn't send anything)
    if (connect(temp_sock, (struct sockaddr*)&peer_state.server_addr, 
                sizeof(peer_state.server_addr)) < 0) {
        perror("Failed to connect temporary socket");
        close(temp_sock);
        return -1;
    }
    
    // Get the local address
    if (getsockname(temp_sock, (struct sockaddr*)&local_addr, &addr_len) < 0) {
        perror("Failed to get local address");
        close(temp_sock);
        return -1;
    }
    close(temp_sock);

    // Register with index server
    struct pdu_register pdu = {0};
    pdu.type = PDU_TYPE_R;
    snprintf(pdu.pName, sizeof(pdu.pName), "peer%d", peer_state.peer_id);
    pdu.pNameLength = strlen(pdu.pName);
    strncpy(pdu.cName, filename, sizeof(pdu.cName) - 1);
    pdu.cNameLength = strlen(filename);
    pdu.address = local_addr.sin_addr.s_addr;
    pdu.port = htons(tcp_port);  // Convert to network byte order

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(local_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
    printf("Registering content with address: %s, port: %d\n", ip_str, tcp_port);

    // Set timeout for registration
    set_socket_timeout(peer_state.udp_sock, 2);

    int retry_count = 0;
    const int max_retries = MAX_RETRIES;
    
    while (retry_count < max_retries) {
        // Send registration request
        if (sendto(peer_state.udp_sock, &pdu, sizeof(pdu), 0,
                   (struct sockaddr*)&peer_state.server_addr,
                   sizeof(peer_state.server_addr)) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                retry_count++;
                usleep(100000); // 100ms delay
                continue;
            }
            perror("Failed to send registration request");
            return -1;
        }

        // Wait for acknowledgment with timeout
        char response;
        fd_set readfds;
        struct timeval tv;
        
        FD_ZERO(&readfds);
        FD_SET(peer_state.udp_sock, &readfds);
        
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        int select_result = select(peer_state.udp_sock + 1, &readfds, NULL, NULL, &tv);
        
        if (select_result > 0) {
            ssize_t recv_result = recv(peer_state.udp_sock, &response, 1, 0);
            if (recv_result > 0) {
                if (response == PDU_TYPE_A) {
                    printf("Content registered successfully\n");
                    add_content(filename, pdu.address, tcp_port);
                    return 0;
                } else if (response == PDU_TYPE_E) {
                    printf("Server rejected registration, probably already registered\n");
                    return -1;
                }
            }
        } else if (select_result == 0) {
            printf("Registration attempt %d timed out\n", retry_count + 1);
        } else {
            perror("Select error");
            return -1;
        }
        
        retry_count++;
        if (retry_count < max_retries) {
            printf("Retrying registration (%d/%d)...\n", retry_count, max_retries);
        }
    }

    printf("Registration failed after %d attempts\n", max_retries);
    return -1;
}

/* File Transfer Functions */
static int send_file(int sock, const char* filename) {
    FILE* file = fopen(filename, "rb");
    if (!file) {
        perror("Failed to open file for sending");
        return -1;
    }

    char buffer[CHUNK_SIZE];
    struct pdu_content pdu;
    size_t bytes_read;
    int retry_count = 0;
    long total_sent = 0;

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        pdu.type = PDU_TYPE_C;
        pdu.length = bytes_read;
        memcpy(pdu.data, buffer, bytes_read);

        size_t total_to_send = bytes_read + sizeof(pdu.type) + sizeof(pdu.length);
        char* send_ptr = (char*)&pdu;
        size_t remaining = total_to_send;

        while (remaining > 0) {
            ssize_t sent = send(sock, send_ptr, remaining, 0);
            if (sent < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (++retry_count > MAX_RETRIES) {
                        fclose(file);
                        return -1;
                    }
                    usleep(100000); // 100ms delay before retry
                    continue;
                }
                perror("Send error");
                fclose(file);
                return -1;
            }
            remaining -= sent;
            send_ptr += sent;
            total_sent += sent;
            retry_count = 0;
        }

        // Print progress
        printf("\rSent: %ld bytes", total_sent);
        fflush(stdout);
    }

    // Send completion marker
    char completion = PDU_TYPE_A;
    if (send(sock, &completion, 1, 0) < 0) {
        perror("Failed to send completion marker");
        fclose(file);
        return -1;
    }

    printf("\nFile transfer completed successfully\n");
    fclose(file);
    return 0;
}

static int receive_file(int sock, const char* filename) {
    FILE* file = fopen(filename, "wb");
    if (!file) {
        perror("Failed to create output file");
        return -1;
    }

    struct pdu_content pdu;
    size_t total_received = 0;
    int retry_count = 0;

    while (1) {
        // First read the PDU header
        ssize_t header_size = sizeof(pdu.type) + sizeof(pdu.length);
        char* recv_ptr = (char*)&pdu;
        size_t remaining = header_size;

        while (remaining > 0) {
            ssize_t received = recv(sock, recv_ptr, remaining, 0);
            if (received < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (++retry_count > MAX_RETRIES) {
                        fclose(file);
                        unlink(filename);  // Delete partial file
                        return -1;
                    }
                    usleep(100000);
                    continue;
                }
                perror("Receive error");
                fclose(file);
                unlink(filename);
                return -1;
            }
            if (received == 0) {  // Connection closed
                fclose(file);
                return 0;
            }
            remaining -= received;
            recv_ptr += received;
            retry_count = 0;
        }

        // Check for completion marker
        if (pdu.type == PDU_TYPE_A) {
            break;
        }

        if (pdu.type != PDU_TYPE_C) {
            fprintf(stderr, "Unexpected PDU type: %c\n", pdu.type);
            fclose(file);
            unlink(filename);
            return -1;
        }

        // Read the actual data
        remaining = pdu.length;
        recv_ptr = pdu.data;

        while (remaining > 0) {
            ssize_t received = recv(sock, recv_ptr, remaining, 0);
            if (received < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (++retry_count > MAX_RETRIES) {
                        fclose(file);
                        unlink(filename);
                        return -1;
                    }
                    usleep(100000);
                    continue;
                }
                perror("Receive error");
                fclose(file);
                unlink(filename);
                return -1;
            }
            remaining -= received;
            recv_ptr += received;
            total_received += received;
            retry_count = 0;
        }

        // Write to file
        if (fwrite(pdu.data, 1, pdu.length, file) != pdu.length) {
            perror("Write error");
            fclose(file);
            unlink(filename);
            return -1;
        }

        printf("\rReceived: %zu bytes", total_received);
        fflush(stdout);
    }

    printf("\nFile transfer completed successfully\n");
    fclose(file);
    return 0;
}

/* TCP Server Implementation */
static int start_tcp_server(const char* filename) {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Failed to create TCP socket");
        return -1;
    }

    // Enable address reuse
    int reuse = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("Failed to set socket reuse option");
        close(server_sock);
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;  // Bind to all interfaces

    // Try to bind to next available port
    int bound = 0;
    uint16_t final_port = 0;
    
    for (int attempts = 0; attempts < 10; attempts++) {
        addr.sin_port = htons(peer_state.next_tcp_port++);
        if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            bound = 1;
            final_port = ntohs(addr.sin_port);
            break;
        }
    }

    if (!bound) {
        perror("Failed to bind TCP socket");
        close(server_sock);
        return -1;
    }

    if (listen(server_sock, 5) < 0) {
        perror("Listen failed");
        close(server_sock);
        return -1;
    }

    printf("TCP server listening on port %d\n", final_port);

    // Fork a child process to handle incoming connections
    pid_t pid = fork();
    if (pid < 0) {
        perror("Fork failed");
        close(server_sock);
        return -1;
    }

    if (pid == 0) {  // Child process
        while (1) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_len);
            
            if (client_sock < 0) {
                if (errno == EINTR) continue;
                perror("Accept failed");
                exit(1);
            }

            // Fork again to handle each client connection
            pid_t client_pid = fork();
            if (client_pid == 0) {
                close(server_sock);
                handle_download_request(client_sock, filename);
                close(client_sock);
                exit(0);
            } else if (client_pid > 0) {
                close(client_sock);
            }
        }
    }

    return final_port;  // Return the actual bound port
}

static void handle_download_request(int client_sock, const char* filename) {
    struct pdu_download request;
    ssize_t received = recv(client_sock, &request, sizeof(request), 0);
    
    if (received < 0 || request.type != PDU_TYPE_D) {
        char error = PDU_TYPE_E;
        send(client_sock, &error, 1, 0);
        return;
    }

    set_socket_timeout(client_sock, TRANSFER_TIMEOUT);
    send_file(client_sock, filename);
}

/* Content Download Implementation */
static int download_content(const char* filename) {
    struct pdu_search search = {0};
    search.type = PDU_TYPE_S;
    snprintf(search.pName, sizeof(search.pName), "peer%d", peer_state.peer_id);
    search.pNameLength = strlen(search.pName);
    strncpy(search.cName, filename, sizeof(search.cName) - 1);
    search.cNameLength = strlen(filename);

    set_socket_timeout(peer_state.udp_sock, 2);
    
    int retry_count = 0;
    const int max_retries = MAX_RETRIES;
    
    while (retry_count < max_retries) {
        if (sendto(peer_state.udp_sock, &search, sizeof(search), 0,
                   (struct sockaddr*)&peer_state.server_addr,
                   sizeof(peer_state.server_addr)) < 0) {
            perror("Failed to send search request");
            return -1;
        }

        fd_set readfds;
        struct timeval tv;
        struct pdu_register response;
        
        FD_ZERO(&readfds);
        FD_SET(peer_state.udp_sock, &readfds);
        
        tv.tv_sec = (rand() % 6) + 2;
        tv.tv_usec = 0;

        // Wait for a response from the server for up to 2 seconds. If we
        // receive a response, select() will return a positive value and we
        // can process the response. If we don't receive a response within 2
        // seconds, select() will return 0 and we can retry the request.
        int select_result = select(peer_state.udp_sock + 1, &readfds, NULL, NULL, &tv);
        
        if (select_result > 0) {
            ssize_t received = recv(peer_state.udp_sock, &response, sizeof(response), 0);
            
            if (received > 0) {
                if (response.type == PDU_TYPE_E) {
                    printf("Content not found\n");
                    return -1;
                }

                int sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock < 0) {
                    perror("Failed to create socket");
                    return -1;
                }

                struct sockaddr_in server_addr = {0};
                server_addr.sin_family = AF_INET;
                server_addr.sin_port = response.port;  // Already in network byte order
                server_addr.sin_addr.s_addr = response.address;

                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(server_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
                printf("Connecting to peer at %s:%d\n", ip_str, ntohs(response.port));

                // Set non-blocking mode
                int flags = fcntl(sock, F_GETFL, 0);
                fcntl(sock, F_SETFL, flags | O_NONBLOCK);

                int connect_result = connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
                if (connect_result < 0) {
                    if (errno == EINPROGRESS) {
                        fd_set writefds;
                        struct timeval connect_tv;
                        
                        FD_ZERO(&writefds);
                        FD_SET(sock, &writefds);
                        
                        connect_tv.tv_sec = 2;
                        connect_tv.tv_usec = 0;

                        select_result = select(sock + 1, NULL, &writefds, NULL, &connect_tv);
                        
                        if (select_result > 0) {
                            int so_error;
                            socklen_t len = sizeof(so_error);
                            getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
                            
                            if (so_error != 0) {
                                printf("Connection error: %s\n", strerror(so_error));
                                close(sock);
                                retry_count++;
                                continue;
                            }
                        } else {
                            printf("Connection timeout\n");
                            close(sock);
                            retry_count++;
                            continue;
                        }
                    } else {
                        perror("Failed to connect to content server");
                        close(sock);
                        retry_count++;
                        continue;
                    }
                }

                // Set back to blocking mode
                fcntl(sock, F_SETFL, flags);

                // Send download request
                struct pdu_download request = {0};
                request.type = PDU_TYPE_D;
                strncpy(request.cName, filename, sizeof(request.cName) - 1);
                request.cNameLength = strlen(filename);

                if (send(sock, &request, sizeof(request), 0) < 0) {
                    perror("Failed to send download request");
                    close(sock);
                    return -1;
                }

                printf("Connected successfully, starting download...\n");
                set_socket_timeout(sock, 2);
                int result = receive_file(sock, filename);
                close(sock);

                if (result == 0) {
                    printf("\n[Download completed successfully]\n\n");
                    register_content(filename);
                    return 0;
                }
            }
        }
        
        retry_count++;
        printf("Retrying download (%d/%d)...\n", retry_count, max_retries);
    }

    printf("Download failed after %d attempts\n", max_retries);
    return -1;
}

/* Content Listing and Deregistration */
static int list_content(void) {
    // Flush any pending data
    static char flush_buffer[MAX_BUFFER_SIZE];
    while (recv(peer_state.udp_sock, flush_buffer, sizeof(flush_buffer), 0) > 0) {}
    
    int retry_count = 0;
    while (retry_count < MAX_RETRIES) {
        char request = PDU_TYPE_O;
        if (sendto(peer_state.udp_sock, &request, 1, 0,
                   (struct sockaddr*)&peer_state.server_addr,
                   sizeof(peer_state.server_addr)) < 0) {
            perror("Failed to send list request");
            retry_count++;
            continue;
        }

        // Buffer for receiving the complete PDU_O structure
        struct {
            char type;
            uint8_t totalEntries;
            struct {
                uint8_t cLength;
                uint8_t pLength;
            } lengths[MAX_CONTENT_SIZE];
            uint8_t dataLength;
            char data[MAX_CONTENT_SIZE * 2];
        } response = {0};

        // Receive type
        if (recv(peer_state.udp_sock, &response.type, 1, 0) < 0) {
            perror("Failed to receive response type");
            retry_count++;
            continue;
        }

        if (response.type != PDU_TYPE_O) {
            printf("Received unexpected response type: %c\n", response.type);
            retry_count++;
            continue;
        }

        // Receive total number of entries
        if (recv(peer_state.udp_sock, &response.totalEntries, 1, 0) < 0) {
            perror("Failed to receive entry count");
            retry_count++;
            continue;
        }

        printf("\nAvailable content (%d entries):\n", response.totalEntries);
        
        if (response.totalEntries > 0) {
            // Receive array of lengths
            size_t lengths_size = response.totalEntries * sizeof(response.lengths[0]);
            if (recv(peer_state.udp_sock, response.lengths, lengths_size, 0) < 0) {
                perror("Failed to receive content and peer lengths");
                retry_count++;
                continue;
            }

            // Receive total data length
            if (recv(peer_state.udp_sock, &response.dataLength, 1, 0) < 0) {
                perror("Failed to receive data length");
                retry_count++;
                continue;
            }

            // Receive content and peer data
            if (recv(peer_state.udp_sock, response.data, response.dataLength, 0) < 0) {
                perror("Failed to receive content and peer data");
                retry_count++;
                continue;
            }

            // Parse and print content entries
            int offset = 0;
            printf("\n%-40s %-20s\n", "Content", "Hosted by");
            printf("%-40s %-20s\n", "-------", "---------");
            
            for (int i = 0; i < response.totalEntries; i++) {
                // Print content name
                printf("%-40.*s ", response.lengths[i].cLength, 
                       &response.data[offset]);
                offset += response.lengths[i].cLength;
                
                // Print peer name
                printf("%-20.*s\n", response.lengths[i].pLength, 
                       &response.data[offset]);
                offset += response.lengths[i].pLength;
            }
        } else {
            printf("No content available\n");
        }

        return 0;
    }

    printf("Failed to retrieve content list after %d attempts\n", retry_count);
    return -1;
}

static int deregister_content(const char* filename) {
    struct pdu_search request = {0};
    request.type = PDU_TYPE_T;
    snprintf(request.pName, sizeof(request.pName), "peer%d", peer_state.peer_id);
    request.pNameLength = strlen(request.pName);
    strncpy(request.cName, filename, sizeof(request.cName) - 1);
    request.cNameLength = strlen(filename);

    int retry_count = 0;
    const int max_retries = MAX_RETRIES;
    
    while (retry_count < max_retries) {
        if (sendto(peer_state.udp_sock, &request, sizeof(request), 0,
                   (struct sockaddr*)&peer_state.server_addr,
                   sizeof(peer_state.server_addr)) < 0) {
            perror("Failed to send deregistration request");
            retry_count++;
            continue;
        }

        // Wait for acknowledgment with timeout
        fd_set readfds;
        struct timeval tv;
        char response;
        
        FD_ZERO(&readfds);
        FD_SET(peer_state.udp_sock, &readfds);
        
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        int select_result = select(peer_state.udp_sock + 1, &readfds, NULL, NULL, &tv);
        
        if (select_result > 0) {
            ssize_t recv_result = recv(peer_state.udp_sock, &response, 1, 0);
            if (recv_result > 0) {
                if (response == PDU_TYPE_A) {
                    remove_content(filename);
                    printf("Content deregistered successfully\n");
                    return 0;
                }
            }
        }
        
        retry_count++;
        printf("Retrying deregistration (%d/%d)...\n", retry_count, max_retries);
    }

    printf("Deregistration failed after %d attempts\n", max_retries);
    return -1;
}

/* Helper Functions Implementation */

static void set_socket_timeout(int sock, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Enable keep-alive
    int keepalive = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) < 0) {
        perror("Failed to set keepalive");
    }

    // Set non-blocking mode
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }
}

static void handle_child_exit(int sig) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0);
}

static void cleanup_and_exit(void) {
    // Deregister all content before exiting
    struct content_host* current = peer_state.hosted_content;
    while (current) {
        deregister_content(current->cName);
        current = current->next;
    }

    // Cleanup content list
    while (peer_state.hosted_content) {
        struct content_host* next = peer_state.hosted_content->next;
        free(peer_state.hosted_content);
        peer_state.hosted_content = next;
    }

    // Close sockets
    close(peer_state.udp_sock);

    printf("Cleanup completed. Exiting...\n");
    exit(0);
}

static struct content_host* find_content(const char* filename) {
    struct content_host* current = peer_state.hosted_content;
    while (current) {
        if (strcmp(current->cName, filename) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

static void add_content(const char* filename, uint32_t addr, uint16_t port) {
    struct content_host* new_content = malloc(sizeof(struct content_host));
    if (!new_content) {
        perror("Memory allocation failed");
        return;
    }

    snprintf(new_content->pName, sizeof(new_content->pName), "peer%d", peer_state.peer_id);
    strncpy(new_content->cName, filename, sizeof(new_content->cName) - 1);
    new_content->cName[sizeof(new_content->cName) - 1] = '\0';
    new_content->address = addr;
    new_content->port = port;
    new_content->next = peer_state.hosted_content;
    peer_state.hosted_content = new_content;
}

static void remove_content(const char* filename) {
    struct content_host** current = &peer_state.hosted_content;
    while (*current) {
        if (strcmp((*current)->cName, filename) == 0) {
            struct content_host* to_remove = *current;
            *current = to_remove->next;
            free(to_remove);
            return;
        }
        current = &(*current)->next;
    }
}
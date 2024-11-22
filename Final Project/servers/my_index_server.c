#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <stdio.h>
#include <time.h>
#include <stdbool.h>
#include <errno.h>
#include <arpa/inet.h>
#include <unistd.h>  // For close()
#include <limits.h>  // For INT_MAX

#define MAX_PDU_SIZE 1024
#define MAX_CONTENT_SIZE 100
#define DEFAULT_PORT 3000

/* PDU Type definitions */
#define PDU_TYPE_R 'R'  // Registration
#define PDU_TYPE_D 'D'  // Download
#define PDU_TYPE_S 'S'  // Search
#define PDU_TYPE_T 'T'  // De-registration
#define PDU_TYPE_C 'C'  // Content
#define PDU_TYPE_O 'O'  // List
#define PDU_TYPE_A 'A'  // Ack
#define PDU_TYPE_E 'E'  // Error

/* PDU Structures with packed attribute to ensure consistent network transmission */
struct __attribute__((__packed__)) pdu_R {
    char type;
    uint8_t pNameLength;
    char pName[MAX_CONTENT_SIZE];
    uint8_t cNameLength;
    char cName[MAX_CONTENT_SIZE];
    uint32_t address;
    uint16_t port;
};

struct __attribute__((__packed__)) pdu_S_T {
    char type;
    uint8_t pNameLength;
    char pName[MAX_CONTENT_SIZE];
    uint8_t cNameLength;
    char cName[MAX_CONTENT_SIZE];
};

struct __attribute__((__packed__)) pdu_O {
    char type;
    uint8_t totalEntries;
    struct {
        uint8_t cLength;  // content name length
        uint8_t pLength;  // peer name length
    } lengths[MAX_CONTENT_SIZE];
    uint8_t dataLength;
    char data[MAX_CONTENT_SIZE * 2];  // Increased to accommodate both content and peer names
};

/* Content host structure */
struct contentHost {
    uint8_t pNameLength;
    char pName[MAX_CONTENT_SIZE];
    uint8_t cNameLength;
    char cName[MAX_CONTENT_SIZE];
    uint32_t address;
    uint16_t port;
    int useCount;  // Track how many times this host has been used
    struct contentHost* next;
};

/* Function prototypes */
static struct contentHost* createHost(uint8_t nLength, const char* name, 
                                    uint8_t cLength, const char* cname, 
                                    uint32_t address, uint16_t port);
static void appendHost(struct contentHost** head, uint8_t nLength, const char* name,
                      uint8_t cLength, const char* cname, uint32_t address, uint16_t port);
static int removeHost(struct contentHost** head, uint8_t nameLength, const char* name,
                     uint8_t cNameLength, const char* cname);
static struct contentHost* findLeastUsedHost(struct contentHost** head, const char* cname, uint8_t cNameLength);
static void sendError(int sock, struct sockaddr_in* addr);
static void sendAck(int sock, struct sockaddr_in* addr);
static void handleRegistration(int sock, struct sockaddr_in* addr, struct contentHost** head);
static void handleDeregistration(int sock, struct sockaddr_in* addr, struct contentHost** head);
static void handleContentList(int sock, struct sockaddr_in* addr, struct contentHost** head);
static void handleContentLookup(int sock, struct sockaddr_in* addr, struct contentHost** head);

/* Global variables */
static struct contentHost* contentList = NULL;

/* Main server implementation */
int main(int argc, char *argv[]) {
    struct sockaddr_in sin, client_addr;
    int sock, port = DEFAULT_PORT;
    socklen_t client_len;
    char buffer[MAX_PDU_SIZE];

    // Process command line arguments
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port number. Using default port %d\n", DEFAULT_PORT);
            port = DEFAULT_PORT;
        }
    }

    // Create UDP socket
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Failed to create socket");
        exit(1);
    }

    // Configure server address
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(port);

    // Bind socket
    if (bind(sock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("Failed to bind socket");
        close(sock);
        exit(1);
    }

    printf("Server listening on port %d\n", port);

    // Main server loop
    while (1) {
        client_len = sizeof(client_addr);
        ssize_t received = recvfrom(sock, buffer, sizeof(buffer), 0,
                                  (struct sockaddr *)&client_addr, &client_len);
        
        if (received < 0) {
            perror("Error receiving data");
            continue;
        }

        if (received < 1) {
            fprintf(stderr, "Received invalid packet size\n");
            continue;
        }

        // Handle different PDU types
        switch (buffer[0]) {
            case PDU_TYPE_R:
                handleRegistration(sock, &client_addr, &contentList);
                break;
            case PDU_TYPE_T:
                handleDeregistration(sock, &client_addr, &contentList);
                break;
            case PDU_TYPE_O:
                handleContentList(sock, &client_addr, &contentList);
                break;
            case PDU_TYPE_S:
                handleContentLookup(sock, &client_addr, &contentList);
                break;
            default:
                fprintf(stderr, "Unknown PDU type: %c\n", buffer[0]);
                sendError(sock, &client_addr);
        }
    }

    return 0;
}

/* Helper function implementations */
static struct contentHost* createHost(uint8_t nLength, const char* name,
                                    uint8_t cLength, const char* cname,
                                    uint32_t address, uint16_t port) {
    struct contentHost* newHost = calloc(1, sizeof(struct contentHost));
    if (!newHost) {
        perror("Memory allocation failed");
        return NULL;
    }

    newHost->pNameLength = nLength;
    newHost->cNameLength = cLength;
    newHost->address = address;
    newHost->port = port;
    newHost->useCount = 0;
    
    memcpy(newHost->pName, name, nLength);
    memcpy(newHost->cName, cname, cLength);
    
    return newHost;
}

static void appendHost(struct contentHost** head, uint8_t nLength, const char* name,
                      uint8_t cLength, const char* cname, uint32_t address, uint16_t port) {
    struct contentHost* newHost = createHost(nLength, name, cLength, cname, address, port);
    if (!newHost) return;

    if (!*head) {
        *head = newHost;
        return;
    }

    struct contentHost* current = *head;
    while (current->next) {
        current = current->next;
    }
    current->next = newHost;
}

static int removeHost(struct contentHost** head, uint8_t nameLength, const char* name,
                     uint8_t cNameLength, const char* cname) {
    if (!*head) return -1;

    struct contentHost* current = *head;
    struct contentHost* prev = NULL;

    while (current) {
        if (current->pNameLength == nameLength &&
            current->cNameLength == cNameLength &&
            memcmp(current->pName, name, nameLength) == 0 &&
            memcmp(current->cName, cname, cNameLength) == 0) {
            
            // If it's the head node
            if (!prev) {
                *head = current->next;
            } else {
                prev->next = current->next;
            }
            
            free(current);
            return 0;
        }
        prev = current;
        current = current->next;
    }

    return -1;  // Host not found
}
static struct contentHost* findExactHost(struct contentHost** head, 
                                       const char* pName, uint8_t pNameLength,
                                       const char* cName, uint8_t cNameLength) {
    struct contentHost* current = *head;
    
    while (current) {
        if (current->pNameLength == pNameLength &&
            current->cNameLength == cNameLength &&
            memcmp(current->pName, pName, pNameLength) == 0 &&
            memcmp(current->cName, cName, cNameLength) == 0) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}
static struct contentHost* findLeastUsedHost(struct contentHost** head, const char* cname, uint8_t cNameLength) {
    if (!*head) return NULL;

    struct contentHost* current = *head;
    struct contentHost* leastUsed = NULL;
    int minUseCount = INT_MAX;

    while (current) {
        if (current->cNameLength == cNameLength && 
            memcmp(current->cName, cname, cNameLength) == 0) {
            // Prefer hosts with lower use count
            if (current->useCount < minUseCount) {
                minUseCount = current->useCount;
                leastUsed = current;
            }
        }
        current = current->next;
    }

    if (leastUsed) {
        leastUsed->useCount++;
    }

    return leastUsed;
}

/* PDU handling functions */
static void handleRegistration(int sock, struct sockaddr_in* addr, struct contentHost** head) {
    struct pdu_R request;
    socklen_t addr_len = sizeof(*addr);
    char buffer[sizeof(struct pdu_R)];

    // Receive registration PDU
    ssize_t received = recvfrom(sock, buffer, sizeof(buffer), 0,
                               (struct sockaddr *)addr, &addr_len);
    
    if (received < sizeof(struct pdu_R)) {
        fprintf(stderr, "Incomplete registration PDU received\n");
        sendError(sock, addr);
        return;
    }

    // Copy buffer to structure
    memcpy(&request, buffer, sizeof(request));

    // Store the client's actual IP address from the UDP packet
    request.address = addr->sin_addr.s_addr;
    
    // Convert port from network byte order to host byte order for storage
    uint16_t host_port = ntohs(request.port);

    // Validate the request
    if (request.pNameLength > MAX_CONTENT_SIZE || request.cNameLength > MAX_CONTENT_SIZE) {
        fprintf(stderr, "Invalid name lengths in registration request\n");
        sendError(sock, addr);
        return;
    }

    // Check if this exact peer+content combination already exists
    struct contentHost* existing = findExactHost(head, request.pName, request.pNameLength,
                                               request.cName, request.cNameLength);
    
    if (existing) {
        fprintf(stderr, "Content already registered by this peer\n");
        sendError(sock, addr);
        return;
    }

    // Add new host with the actual client address and port in host byte order
    appendHost(head, request.pNameLength, request.pName,
              request.cNameLength, request.cName,
              request.address, host_port);  // Store port in host byte order
    
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(addr->sin_addr), ip_str, INET_ADDRSTRLEN);
    
    printf("Successfully registered content: %.*s from peer: %.*s (address: %s, port: %u)\n",
           request.cNameLength, request.cName,
           request.pNameLength, request.pName,
           ip_str, host_port);
    
    sendAck(sock, addr);
}

/* Handler function for content deregistration */
static void handleDeregistration(int sock, struct sockaddr_in* addr, struct contentHost** head) {
    struct pdu_S_T request;
    socklen_t addr_len = sizeof(*addr);

    // Receive deregistration PDU
    ssize_t received = recvfrom(sock, &request, sizeof(request), 0,
                               (struct sockaddr *)addr, &addr_len);
    
    if (received < sizeof(struct pdu_S_T)) {
        fprintf(stderr, "Incomplete deregistration PDU received\n");
        sendError(sock, addr);
        return;
    }

    // Validate lengths
    if (request.pNameLength > MAX_CONTENT_SIZE || request.cNameLength > MAX_CONTENT_SIZE) {
        fprintf(stderr, "Invalid name lengths in deregistration request\n");
        sendError(sock, addr);
        return;
    }

    // Attempt to remove the host
    int result = removeHost(head, request.pNameLength, request.pName,
                          request.cNameLength, request.cName);
    
    if (result == 0) {
        printf("Successfully deregistered content: %.*s from peer: %.*s\n",
               request.cNameLength, request.cName,
               request.pNameLength, request.pName);
        sendAck(sock, addr);
    } else {
        fprintf(stderr, "Failed to deregister content - host not found\n");
        sendError(sock, addr);
    }
}

/* Handler function for content listing */
static void handleContentList(int sock, struct sockaddr_in* addr, struct contentHost** head) {
    struct pdu_O response;
    struct contentHost* current = *head;
    int totalSize = 0;
    int entryCount = 0;
    
    // Initialize response
    memset(&response, 0, sizeof(response));
    response.type = PDU_TYPE_O;

    // Temporary buffers to track what we've already listed
    struct {
        char content[MAX_CONTENT_SIZE];
        int cLength;
        char peer[MAX_CONTENT_SIZE];
        int pLength;
    } entries[MAX_CONTENT_SIZE];
    int uniqueEntries = 0;

    // First pass: gather all unique content-peer combinations
    while (current != NULL && uniqueEntries < MAX_CONTENT_SIZE) {
        // Add this content-peer combination to our list
        memcpy(entries[uniqueEntries].content, current->cName, current->cNameLength);
        entries[uniqueEntries].cLength = current->cNameLength;
        memcpy(entries[uniqueEntries].peer, current->pName, current->pNameLength);
        entries[uniqueEntries].pLength = current->pNameLength;
        
        // Check if adding these lengths would exceed our buffer
        if (totalSize + current->cNameLength + current->pNameLength > sizeof(response.data)) {
            fprintf(stderr, "Content list would exceed maximum size\n");
            sendError(sock, addr);
            return;
        }

        // Copy content name length and peer name length
        response.lengths[uniqueEntries].cLength = current->cNameLength;
        response.lengths[uniqueEntries].pLength = current->pNameLength;

        // Copy content name and peer name to data buffer
        memcpy(response.data + totalSize, current->cName, current->cNameLength);
        totalSize += current->cNameLength;
        memcpy(response.data + totalSize, current->pName, current->pNameLength);
        totalSize += current->pNameLength;

        uniqueEntries++;
        current = current->next;
    }

    response.totalEntries = uniqueEntries;
    response.dataLength = totalSize;

    // Send the complete response structure
    // First send type
    if (sendto(sock, &response.type, 1, 0,
               (struct sockaddr *)addr, sizeof(*addr)) < 0) {
        perror("Failed to send content list type");
        return;
    }

    // Send number of entries
    if (sendto(sock, &response.totalEntries, 1, 0,
               (struct sockaddr *)addr, sizeof(*addr)) < 0) {
        perror("Failed to send entry count");
        return;
    }

    if (uniqueEntries > 0) {
        // Send array of lengths (both content and peer lengths for each entry)
        size_t lengths_size = uniqueEntries * sizeof(response.lengths[0]);
        if (sendto(sock, response.lengths, lengths_size, 0,
                   (struct sockaddr *)addr, sizeof(*addr)) < 0) {
            perror("Failed to send content and peer lengths");
            return;
        }

        // Send total data length
        if (sendto(sock, &response.dataLength, 1, 0,
                   (struct sockaddr *)addr, sizeof(*addr)) < 0) {
            perror("Failed to send total data length");
            return;
        }

        // Send the actual data containing both content and peer names
        if (sendto(sock, response.data, totalSize, 0,
                   (struct sockaddr *)addr, sizeof(*addr)) < 0) {
            perror("Failed to send content and peer data");
            return;
        }
    }

    printf("Successfully sent list of %d content entries\n", uniqueEntries);
}

/* Handler function for content lookup */
static void handleContentLookup(int sock, struct sockaddr_in* addr, struct contentHost** head) {
    struct pdu_S_T request;
    socklen_t addr_len = sizeof(*addr);
    char buffer[sizeof(struct pdu_S_T)];

    // Receive lookup PDU
    ssize_t received = recvfrom(sock, buffer, sizeof(buffer), 0,
                               (struct sockaddr *)addr, &addr_len);
    
    if (received < sizeof(struct pdu_S_T)) {
        fprintf(stderr, "Incomplete lookup PDU received\n");
        sendError(sock, addr);
        return;
    }

    // Copy buffer to structure
    memcpy(&request, buffer, sizeof(request));

    // Find the least used host that has the requested content
    struct contentHost* host = findLeastUsedHost(head, request.cName, request.cNameLength);
    
    if (!host) {
        fprintf(stderr, "Content not found: %.*s\n", request.cNameLength, request.cName);
        sendError(sock, addr);
        return;
    }

    // Prepare and send response
    struct pdu_R response;
    memset(&response, 0, sizeof(response));
    response.type = PDU_TYPE_R;
    response.pNameLength = host->pNameLength;
    response.cNameLength = host->cNameLength;
    response.address = host->address;
    response.port = htons(host->port);  // Convert port to network byte order
    
    memcpy(response.pName, host->pName, host->pNameLength);
    memcpy(response.cName, host->cName, host->cNameLength);

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(host->address), ip_str, INET_ADDRSTRLEN);
    
    printf("Found content %.*s at peer %.*s (address: %s, port: %u)\n",
           response.cNameLength, response.cName,
           response.pNameLength, response.pName,
           ip_str, (host->port));
    
    //sleep(1);

    // Send complete response structure
    if (sendto(sock, &response, sizeof(response), 0,
               (struct sockaddr *)addr, sizeof(*addr)) < 0) {
        perror("Failed to send lookup response");
        return;
    }
}
/* Add other handler implementations similarly... */

static void sendError(int sock, struct sockaddr_in* addr) {
    char error = PDU_TYPE_E;
    sendto(sock, &error, 1, 0, (struct sockaddr *)addr, sizeof(*addr));
}

static void sendAck(int sock, struct sockaddr_in* addr) {
    char ack = PDU_TYPE_A;
    sendto(sock, &ack, 1, 0, (struct sockaddr *)addr, sizeof(*addr));
}
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2def.h>
    #include <ws2tcpip.h>
    
    typedef SOCKET socket_t;
#else
    #include <unistd.h>
    #include <arpa/inet.h>
    #include <sys/socket_t.h>
    #include <netinet/in.h>
    #include <netdb.h>

    typedef int socket_t;
    #define INVALID_SOCKET (-1)
    #define SOCKET_ERROR   (-1)
#endif

typedef struct {
    int slot;
    uint32_t server_id;
    uint8_t mac[6];
    uint32_t last_sequence;
    uint64_t last_timestamp;
    bool have_sequence;
    bool subscribed;
    double acc[3];
    double gyro[3];
} DsuClientState;

void dsu_close(socket_t* sock);
int dsu_init(socket_t* sock, struct sockaddr_in* server_addr);
void dsu_send_requests(socket_t sock, const struct sockaddr_in* addr, int slot);
void dsu_process_incoming_packets(socket_t sock, DsuClientState* state);

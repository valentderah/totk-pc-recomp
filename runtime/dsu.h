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
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <fcntl.h>
    #include <errno.h>

    typedef int socket_t;
    #define SOCKET_ERROR   (-1)
    #define INVALID_SOCKET (-1)
#endif

typedef struct {
    int slot;
    uint32_t server_id;
    uint8_t mac[6];
    uint64_t last_timestamp;
    uint64_t last_received_ns;
    double acc[3];
    double gyro[3];
} DsuClientState;

void dsu_close(socket_t* sock);
int dsu_open(socket_t* sock, struct sockaddr_in* server_addr);
// Transform the DSU coordinate system to the one we use for motion fusion.
void dsu_axes(const double a[3], const double g[3], double acc[3], double gyro[3]);
// Request the DSU server to keep sending data back for slot 0. This should be
// consistently called every few seconds.
void dsu_send_request(socket_t sock, const struct sockaddr_in* addr);
void dsu_process_incoming_packets(socket_t sock, DsuClientState* state);

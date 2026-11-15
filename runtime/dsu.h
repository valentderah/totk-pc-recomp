#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <winsock2.h>
#include <ws2def.h>

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

int dsu_init(SOCKET* sock, SOCKADDR_IN* server_addr);
void dsu_send_requests(SOCKET sock, const SOCKADDR_IN* addr, int slot);
void dsu_process_incoming_packets(SOCKET sock, DsuClientState* state);

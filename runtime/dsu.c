#include "dsu.h"
#include "host.h"
#include "cfg.h"

#include <winsock2.h>
#include <ws2def.h>
#include <ws2tcpip.h>
#include <SDL3/SDL_sensor.h>

#define DSU_CLIENT_ID 0x12345678
#define DSU_VERSION 1001
#define DSU_TYPE_VERSION 0x00100000
#define DSU_TYPE_INFO    0x00100001
#define DSU_TYPE_DATA    0x00100002
#define DSU_MAX_PACKET 2048

#define PI 3.14159265358979323846

typedef struct {
    uint32_t type;
    uint32_t server;

    uint8_t slot;
    uint8_t state;
    uint8_t model;
    uint8_t mac[6];

    bool connected;

    uint32_t sequence;
    uint64_t timestamp;

    double acc[3];
    double gyro[3];
} DsuPacket;

int dsu_init(SOCKET* sock, SOCKADDR_IN* server_addr) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        rt_logf("Winsock initialization failed.\n");
        return 1;
    }

    *sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (*sock == INVALID_SOCKET) {
        rt_logf("Socket creation failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    uint16_t port = (uint16_t) cfg_int("DSU_PORT");
    rt_logf("DSU client starting on 127.0.0.1:%u\n", port);

    *server_addr = (SOCKADDR_IN){0};
    server_addr->sin_family = AF_INET;
    server_addr->sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &server_addr->sin_addr);

    u_long nonblocking = 1;
    if (ioctlsocket(*sock, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        rt_logf("Failed to set non-blocking socket.\n");
        closesocket(*sock);
        WSACleanup();
        return 1;
    }

    return 0;
}

static uint16_t read_le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t* p) {
    return read_le16(p) | ((uint32_t)read_le16(p + 2) << 16);
}

static uint64_t read_le64(const uint8_t* p) {
    return read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

static void write_le32(uint8_t* p, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(value >> (8 * i));
    }
}

static float read_lefloat(const uint8_t* p) {
    uint32_t bits = read_le32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t dsu_crc(const uint8_t* p, size_t n) {
    uint32_t crc = ~0u;
    for (size_t i = 0; i < n; i++) {
        crc ^= (i >= 8 && i < 12) ? 0 : p[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
        }
    }
    return ~crc;
}

static size_t dsu_request(uint8_t out[28], uint32_t client, uint32_t type, int slot) {
    size_t size = (type == DSU_TYPE_VERSION) ? 20 : 28;
    memset(out, 0, 28);
    memcpy(out, "DSUC", 4);

    out[4] = 0xe9;
    out[5] = 0x03;
    out[6] = (uint8_t)(size - 16);

    write_le32(out + 12, client);
    write_le32(out + 16, type);

    if (type == DSU_TYPE_INFO) {
        write_le32(out + 20, 4);
        for (int i = 0; i < 4; i++) {
            out[24 + i] = (uint8_t)i;
        }
    } else if (type == DSU_TYPE_DATA) {
        out[20] = 1;
        out[21] = (uint8_t)slot;
    }

    write_le32(out + 8, dsu_crc(out, size));
    return size;
}

static bool send_dsu_type(SOCKET sock, const SOCKADDR_IN* addr, uint32_t type, int slot) {
    uint8_t request[28];
    size_t len = dsu_request(request, DSU_CLIENT_ID, type, slot);
    int sent = sendto(sock, (const char*)request, (int)len, 0, (const struct sockaddr*)addr, sizeof(*addr));
    return sent != SOCKET_ERROR;
}

void dsu_send_requests(SOCKET sock, const SOCKADDR_IN* addr, int slot) {
    send_dsu_type(sock, addr, DSU_TYPE_VERSION, 0);
    send_dsu_type(sock, addr, DSU_TYPE_INFO, 0);
    if (slot >= 0) {
        send_dsu_type(sock, addr, DSU_TYPE_DATA, slot);
    }
}

static double deg_to_rad(double deg) {
    return deg * PI / 180.0;
}

static bool dsu_decode(const uint8_t* p, size_t n, DsuPacket* out) {
    if (n < 20 || memcmp(p, "DSUS", 4) != 0 || read_le16(p + 4) != DSU_VERSION) {
        return false;
    }

    size_t size = 16 + read_le16(p + 6);
    if (size < 20 || size > n || read_le32(p + 8) != dsu_crc(p, size)) {
        return false;
    }

    DsuPacket v = { 0 };
    v.type = read_le32(p + 16);
    v.server = read_le32(p + 12);

    if (v.type == DSU_TYPE_INFO) {
        if (size != 32 || p[20] > 3 || p[21] > 2) return false;
        v.slot = p[20];
        v.state = p[21];
        v.model = p[22];
        memcpy(v.mac, p + 24, 6);
    } else if (v.type == DSU_TYPE_DATA) {
        if (size != 100 || p[20] > 3 || p[21] > 2) return false;
        v.slot = p[20];
        v.state = p[21];
        v.model = p[22];
        memcpy(v.mac, p + 24, 6);

        v.connected = (p[31] == 1 && v.state == 2);
        v.sequence = read_le32(p + 32);
        v.timestamp = read_le64(p + 68);

        double a[3], w[3];
        for (int i = 0; i < 3; i++) {
            a[i] = read_lefloat(p + 76 + 4 * i);
            w[i] = read_lefloat(p + 88 + 4 * i);

            if (!isfinite(a[i]) || !isfinite(w[i]) || fabs(a[i]) > 32 || fabs(w[i]) > 10000) {
                return false;
            }
        }

        v.acc[0] = a[0];
        v.acc[1] = -a[2];
        v.acc[2] = a[1];
        v.gyro[0] = deg_to_rad(w[0]);
        v.gyro[1] = deg_to_rad(w[2]);
        v.gyro[2] = -deg_to_rad(w[1]);
    } else {
        return false;
    }

    *out = v;
    return true;
}

static void handle_info_packet(DsuClientState* state, const DsuPacket* packet) {
    if (packet->state == 2 && packet->model == 2) {
        if (state->slot != packet->slot ||
            state->server_id != packet->server ||
            memcmp(state->mac, packet->mac, 6) != 0) {

            state->slot = packet->slot;
            state->server_id = packet->server;
            memcpy(state->mac, packet->mac, 6);

            state->have_sequence = false;
            state->subscribed = false;

            rt_logf("\nController detected!\n");
        }
    } else if (state->slot == packet->slot && packet->state != 2) {
        rt_logf("\nController disconnected.\n");
        state->slot = -1;
        state->subscribed = false;
        state->have_sequence = false;
    }
}

static void handle_data_packet(DsuClientState* state, const DsuPacket* packet) {
    if (state->slot < 0 ||
        packet->slot != state->slot ||
        packet->server != state->server_id ||
        memcmp(state->mac, packet->mac, 6) != 0) {
        return;
    }

    if (!packet->connected) {
        rt_logf("\nController data disconnected.\n");
        state->have_sequence = false;
        return;
    }

    if (state->have_sequence) {
        uint32_t delta = packet->sequence - state->last_sequence;
        if (delta == 0 || delta >= 0x80000000u || packet->timestamp <= state->last_timestamp) {
            return;
        }
    }

    double acc_norm = packet->acc[0] * packet->acc[0] +
                      packet->acc[1] * packet->acc[1] +
                      packet->acc[2] * packet->acc[2];
    if (acc_norm < 0.01) return;

    state->last_sequence = packet->sequence;
    state->last_timestamp = packet->timestamp;
    state->have_sequence = true;
    state->subscribed = true;
    memcpy(state->acc, packet->acc, sizeof(state->acc));
    memcpy(state->gyro, packet->gyro, sizeof(state->gyro));
}

void dsu_process_incoming_packets(SOCKET sock, DsuClientState* state) {
    uint8_t buffer[DSU_MAX_PACKET];

    while (1) {
        int bytes = recvfrom(sock, (char*)buffer, sizeof(buffer), 0, NULL, NULL);
        if (bytes == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK) {
                rt_logf("Receive error: %d\n", err);
            }
            break;
        }

        DsuPacket packet;
        if (!dsu_decode(buffer, (size_t)bytes, &packet)) {
            continue;
        }

        if (packet.type == DSU_TYPE_INFO) {
            handle_info_packet(state, &packet);
        } else if (packet.type == DSU_TYPE_DATA) {
            handle_data_packet(state, &packet);
        }
    }
}

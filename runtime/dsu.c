#include "dsu.h"
#include "host.h"
#include "cfg.h"

#define DSU_CLIENT_ID 0x06951209
#define DSU_VERSION 1001
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

void dsu_close(socket_t* sock) {
#ifdef _WIN32
    closesocket(*sock);
    WSACleanup();
#else
    close(*sock);
#endif
}

int dsu_open(socket_t* sock, struct sockaddr_in* server_addr) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        rt_log("dsu: winsock initialization failed");
        return 1;
    }
#endif

    *sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (*sock == INVALID_SOCKET) {
        rt_log("dsu: socket_t creation failed.");
        #ifdef _WIN32
            WSACleanup();
        #endif
        return 1;
    }

    uint16_t port = (uint16_t) cfg_int("DSU_PORT");
    rt_logf("dsu: client starting on 127.0.0.1:%u\n", port);

    *server_addr = (struct sockaddr_in){0};
    server_addr->sin_family = AF_INET;
    server_addr->sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr->sin_addr) <= 0) {
        dsu_close(sock);
        return 1;
    }

    // make it nonblocking
#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(*sock, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        rt_log("dsu: failed to set non-blocking socket_t");
        dsu_close(sock);
        return 1;
    }
#else
    int flags = fcntl(*sock, F_GETFL, 0);
    if (flags < 0 || fcntl(*sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        rt_log("dsu: failed to set non-blocking socket_t");
        dsu_close(sock);
        return 1;
    }
#endif

    return 0;
}

void dsu_axes(const double a[3], const double g[3], double acc[3], double gyro[3]) {
    acc[0] = a[0];
    acc[1] = -a[2];
    acc[2] = a[1];
    gyro[0] = g[0] * PI / 180.0;
    gyro[1] = g[2] * PI / 180.0;
    gyro[2] = -g[1] * PI / 180.0;
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

static void dsu_request(uint8_t out[28], uint32_t client, uint32_t type, int slot) {
    memset(out, 0, 28);
    memcpy(out, "DSUC", 4);

    out[4] = 0xe9;
    out[5] = 0x03;
    out[6] = (uint8_t)(28 - 16);

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

    write_le32(out + 8, dsu_crc(out, 28));
}

static bool send_dsu_type(socket_t sock, const struct sockaddr_in* addr, uint32_t type, int slot) {
    uint8_t request[28];
    dsu_request(request, DSU_CLIENT_ID, type, slot);
    int sent = sendto(sock, (const char*)request, 28, 0, (const struct sockaddr*)addr, sizeof(*addr));
    return sent != SOCKET_ERROR;
}

void dsu_send_request(socket_t sock, const struct sockaddr_in* addr) {
    send_dsu_type(sock, addr, DSU_TYPE_DATA, 0);
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

        for (int i = 0; i < 3; i++) {
            v.acc[i] = read_lefloat(p + 76 + 4 * i);
            v.gyro[i] = read_lefloat(p + 88 + 4 * i);
        }
    } else {
        return false;
    }

    *out = v;
    return true;
}

static void handle_data_packet(DsuClientState* state, const DsuPacket* packet) {
    if (state->slot < 0 && packet->state == 2 && packet->model == 2 && packet->slot == 0) {
        // only accept slot 0
        state->slot = packet->slot;
        state->server_id = packet->server;
        memcpy(state->mac, packet->mac, 6);
        rt_logf("dsu: controller detected (slot %d)\n", state->slot);
    } else if (state->slot == packet->slot &&
               (packet->state != 2 || packet->server != state->server_id || memcmp(state->mac, packet->mac, 6) != 0)) {
        rt_log("dsu: controller disconnected.");
        state->slot = -1;
    }
    if (state->slot < 0 ||
        packet->slot != state->slot ||
        packet->server != state->server_id ||
        memcmp(state->mac, packet->mac, 6) != 0) {
        return;
    }

    if (!packet->connected) {
        rt_log("dsu: controller data disconnected.");
        state->slot = -1;
        return;
    }

    if (packet->timestamp <= state->last_timestamp) {
        return;
    }

    state->last_timestamp = packet->timestamp;
    state->last_received_ns = kern_now_ns();
    memcpy(state->acc, packet->acc, sizeof(state->acc));
    memcpy(state->gyro, packet->gyro, sizeof(state->gyro));
}

void dsu_process_incoming_packets(socket_t sock, DsuClientState* state) {
    if (kern_now_ns() - state->last_received_ns >= 2e9) {
        rt_log("dsu: motion timeout.");
        state->slot = -1;
    }

    uint8_t buffer[DSU_MAX_PACKET];

    while (1) {
        int bytes = recvfrom(sock, (char*)buffer, sizeof(buffer), 0, NULL, NULL);
        if (bytes == SOCKET_ERROR) {
#ifdef _WIN32
            int err = WSAGetLastError();
            // WSAECONNRESET means the UDP server does not exist. It's OK.
            if (err != WSAEWOULDBLOCK && err != WSAECONNRESET) {
                rt_logf("dsu: receive error: %d\n", err);
            }
#else
            int err = errno;
            if (err != EAGAIN && err != EWOULDBLOCK) {
                rt_logf("dsu: receive error: %d\n", err);
            }
#endif
            break;
        }

        DsuPacket packet;
        if (!dsu_decode(buffer, (size_t)bytes, &packet)) {
            continue;
        }

        if (packet.type == DSU_TYPE_DATA) {
            handle_data_packet(state, &packet);
        }
    }
}

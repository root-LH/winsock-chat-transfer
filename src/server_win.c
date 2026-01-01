#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#pragma comment(lib, "Ws2_32.lib")

#define BACKLOG 64
#define RELAY_BUF_SZ (256 * 1024)
#define SOCKBUF_SZ   (16 * 1024 * 1024)

#define MSG_TEXT 1

typedef struct {
    SOCKET a;
    SOCKET b;
} pair_t;

static CRITICAL_SECTION g_wait_cs;
static SOCKET g_waiting = INVALID_SOCKET;

static void die(const char *msg) {
    fprintf(stderr, "%s (WSA error=%d)\n", msg, WSAGetLastError());
    exit(1);
}

static int send_all(SOCKET s, const void *buf, int len) {
    const char *p = (const char *)buf;
    int sent = 0;
    while (sent < len) {
        int n = send(s, p + sent, len - sent, 0);
        if (n == SOCKET_ERROR) return -1;
        if (n == 0) break;
        sent += n;
    }
    return sent;
}

static int send_frame(SOCKET s, uint8_t type, const void *payload, uint32_t len) {
    uint32_t net_len = htonl(len);
    if (send_all(s, &type, 1) < 0) return -1;
    if (send_all(s, &net_len, 4) < 0) return -1;
    if (len > 0 && send_all(s, payload, (int)len) < 0) return -1;
    return 0;
}

static void send_system(SOCKET s, const char *msg) {
    send_frame(s, MSG_TEXT, msg, (uint32_t)strlen(msg));
}

static void tune_socket(SOCKET s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    int sz = SOCKBUF_SZ;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&sz, sizeof(sz));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&sz, sizeof(sz));
}

static DWORD WINAPI relay_thread(LPVOID arg) {
    SOCKET *fds = (SOCKET *)arg;
    SOCKET from = fds[0];
    SOCKET to   = fds[1];
    free(fds);

    char *buf = (char*)malloc(RELAY_BUF_SZ);
    if (!buf) {
        shutdown(to, SD_SEND);
        return 0;
    }

    for (;;) {
        int n = recv(from, buf, RELAY_BUF_SZ, 0);
        if (n == 0) {
            send_system(to, "[system] Peer disconnected.");
            shutdown(to, SD_SEND);
            break;
        }
        if (n == SOCKET_ERROR) {
            send_system(to, "[system] Connection error.");
            shutdown(to, SD_SEND);
            break;
        }
        if (send_all(to, buf, n) < 0) {
            shutdown(to, SD_SEND);
            break;
        }
    }

    free(buf);
    return 0;
}

static DWORD WINAPI pair_worker(LPVOID arg) {
    pair_t *p = (pair_t *)arg;
    SOCKET a = p->a;
    SOCKET b = p->b;
    free(p);

    tune_socket(a);
    tune_socket(b);

    send_system(a, "[system] Match complete. You can start chatting and sending files.");
    send_system(a, "[system] File command: /send <path>");
    send_system(b, "[system] Match complete. You can start chatting and sending files.");
    send_system(b, "[system] File command: /send <path>");

    SOCKET *ab = (SOCKET *)malloc(sizeof(SOCKET) * 2);
    SOCKET *ba = (SOCKET *)malloc(sizeof(SOCKET) * 2);
    if (!ab || !ba) {
        closesocket(a);
        closesocket(b);
        free(ab);
        free(ba);
        return 0;
    }

    ab[0] = a; ab[1] = b;
    ba[0] = b; ba[1] = a;

    HANDLE t1 = CreateThread(NULL, 0, relay_thread, ab, 0, NULL);
    HANDLE t2 = CreateThread(NULL, 0, relay_thread, ba, 0, NULL);

    WaitForSingleObject(t1, INFINITE);
    WaitForSingleObject(t2, INFINITE);
    CloseHandle(t1);
    CloseHandle(t2);

    closesocket(a);
    closesocket(b);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port\n");
        return 1;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        die("WSAStartup failed");

    InitializeCriticalSection(&g_wait_cs);

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET)
        die("socket failed");

    BOOL yes = TRUE;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((u_short)port);

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
        die("bind failed");

    if (listen(listen_sock, BACKLOG) == SOCKET_ERROR)
        die("listen failed");

    printf("[server] Listening on port %d\n", port);

    for (;;) {
        struct sockaddr_in caddr;
        int clen = sizeof(caddr);
        SOCKET c = accept(listen_sock, (struct sockaddr *)&caddr, &clen);
        if (c == INVALID_SOCKET) {
            fprintf(stderr, "[server] accept failed (%d)\n", WSAGetLastError());
            continue;
        }

        tune_socket(c);

        char ip[64];
        inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
        printf("[server] Client connected: %s:%d\n", ip, ntohs(caddr.sin_port));

        EnterCriticalSection(&g_wait_cs);
        if (g_waiting == INVALID_SOCKET) {
            g_waiting = c;
            LeaveCriticalSection(&g_wait_cs);
            send_system(c, "[system] Waiting for a peer...");
        } else {
            SOCKET other = g_waiting;
            g_waiting = INVALID_SOCKET;
            LeaveCriticalSection(&g_wait_cs);

            pair_t *p = (pair_t *)malloc(sizeof(pair_t));
            if (!p) {
                closesocket(c);
                closesocket(other);
                continue;
            }
            p->a = other;
            p->b = c;

            HANDLE h = CreateThread(NULL, 0, pair_worker, p, 0, NULL);
            CloseHandle(h);
        }
    }

    closesocket(listen_sock);
    DeleteCriticalSection(&g_wait_cs);
    WSACleanup();
    return 0;
}

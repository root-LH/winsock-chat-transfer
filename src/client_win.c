#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <stdarg.h>

#ifdef _MSC_VER
#include <mswsock.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Mswsock.lib")
#else
#pragma comment(lib, "Ws2_32.lib")
#endif

#define MSG_TEXT       1
#define MSG_FILE_BEGIN 2
#define MSG_FILE_CHUNK 3

#define CHUNK_SZ    (4 * 1024 * 1024)
#define IO_BUF_SZ   (256 * 1024)
#define SOCKBUF_SZ  (16 * 1024 * 1024)
#define PROGRESS_MS 200

#define RATE_UPDATE_MS 500
#define FILE_IOBUF_SZ (8 * 1024 * 1024)

static CRITICAL_SECTION g_print_cs;

static void pprintf(const char *fmt, ...) {
    EnterCriticalSection(&g_print_cs);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    LeaveCriticalSection(&g_print_cs);
}

static void trim_newline(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

static uint64_t htonll_u64(uint64_t x) {
    uint32_t hi = (uint32_t)(x >> 32);
    uint32_t lo = (uint32_t)(x & 0xFFFFFFFFu);
    uint64_t nhi = (uint64_t)htonl(hi);
    uint64_t nlo = (uint64_t)htonl(lo);
    return (nlo << 32) | nhi;
}

static uint64_t ntohll_u64(uint64_t x) {
    uint32_t hi = (uint32_t)(x >> 32);
    uint32_t lo = (uint32_t)(x & 0xFFFFFFFFu);
    uint64_t hhi = (uint64_t)ntohl(hi);
    uint64_t hlo = (uint64_t)ntohl(lo);
    return (hlo << 32) | hhi;
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

static int recv_all(SOCKET s, void *buf, int len) {
    char *p = (char *)buf;
    int recvd = 0;
    while (recvd < len) {
        int n = recv(s, p + recvd, len - recvd, 0);
        if (n <= 0) return -1;
        recvd += n;
    }
    return recvd;
}

static int send_frame_header(SOCKET s, uint8_t type, uint32_t len) {
    uint32_t net_len = htonl(len);
    if (send_all(s, &type, 1) < 0) return -1;
    if (send_all(s, &net_len, 4) < 0) return -1;
    return 0;
}

static int send_frame(SOCKET s, uint8_t type, const void *payload, uint32_t len) {
    uint32_t net_len = htonl(len);
    if (send_all(s, &type, 1) < 0) return -1;
    if (send_all(s, &net_len, 4) < 0) return -1;
    if (len > 0 && send_all(s, payload, (int)len) < 0) return -1;
    return 0;
}

static int send_text(SOCKET s, const char *text) {
    return send_frame(s, MSG_TEXT, text, (uint32_t)strlen(text));
}

static const char *basename_win(const char *path) {
    const char *p1 = strrchr(path, '\\');
    const char *p2 = strrchr(path, '/');
    const char *p = p1 > p2 ? p1 : p2;
    return p ? p + 1 : path;
}

static void make_recv_name(char *out, size_t out_sz, const char *orig) {
    snprintf(out, out_sz, "recv_%s", orig);
    for (char *p = out; *p; ++p) {
        if (strchr("<>:\"|?*", *p)) *p = '_';
    }
}

static const char *skip_spaces(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static int parse_quoted_path(const char *line, char *out, size_t out_sz) {
    const char *p = skip_spaces(line);
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < out_sz) out[i++] = *p++;
        out[i] = 0;
        if (*p != '"') return 0;
        return 1;
    } else {
        size_t i = 0;
        while (*p && i + 1 < out_sz) out[i++] = *p++;
        out[i] = 0;
        size_t n = strlen(out);
        while (n && isspace((unsigned char)out[n - 1])) out[--n] = 0;
        return n > 0;
    }
}

static int is_send_command(const char *line, char *path_out, size_t path_out_sz) {
    if (strncmp(line, "/send", 5) != 0) return 0;
    return parse_quoted_path(line + 5, path_out, path_out_sz);
}

static int send_file_chunked(SOCKET s, const char *path) {
#ifdef _MSC_VER
    HANDLE hFile = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        pprintf("[system] Failed to open file: %s\n", path);
        return -1;
    }

    LARGE_INTEGER liSize;
    if (!GetFileSizeEx(hFile, &liSize)) {
        CloseHandle(hFile);
        pprintf("[system] Failed to get file size\n");
        return -1;
    }

    uint64_t fsize = (uint64_t)liSize.QuadPart;
#else
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        pprintf("[system] Failed to open file: %s\n", path);
        return -1;
    }
    _fseeki64(fp, 0, SEEK_END);
    uint64_t fsize = (uint64_t)_ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);
#endif

    const char *fname = basename_win(path);
    uint16_t fname_len = (uint16_t)strlen(fname);

    uint32_t begin_len = (uint32_t)(2u + fname_len + 8u);
    uint8_t *begin_payload = (uint8_t*)malloc(begin_len);
    if (!begin_payload) {
#ifdef _MSC_VER
        CloseHandle(hFile);
#else
        fclose(fp);
#endif
        return -1;
    }

    uint16_t net_fn = htons(fname_len);
    uint64_t net_sz = htonll_u64(fsize);

    memcpy(begin_payload + 0, &net_fn, 2);
    memcpy(begin_payload + 2, fname, fname_len);
    memcpy(begin_payload + 2 + fname_len, &net_sz, 8);

    if (send_frame(s, MSG_FILE_BEGIN, begin_payload, begin_len) < 0) {
        free(begin_payload);
#ifdef _MSC_VER
        CloseHandle(hFile);
#else
        fclose(fp);
#endif
        return -1;
    }
    free(begin_payload);

    uint64_t sent = 0;
    uint64_t offset = 0;

    ULONGLONG last_print = 0;
    ULONGLONG start_time = GetTickCount64();

    while (offset < fsize) {
        uint64_t remain = fsize - offset;
        uint32_t chunk = (remain > (uint64_t)CHUNK_SZ) ? (uint32_t)CHUNK_SZ : (uint32_t)remain;

        if (send_frame_header(s, MSG_FILE_CHUNK, chunk) < 0) {
#ifdef _MSC_VER
            CloseHandle(hFile);
#else
            fclose(fp);
#endif
            return -1;
        }

#ifdef _MSC_VER
        LARGE_INTEGER liOff;
        liOff.QuadPart = (LONGLONG)offset;
        if (!SetFilePointerEx(hFile, liOff, NULL, FILE_BEGIN)) {
            CloseHandle(hFile);
            return -1;
        }

        if (!TransmitFile(s, hFile, chunk, 0, NULL, NULL, 0)) {
            int err = (int)GetLastError();
            pprintf("\n[system] TransmitFile failed (GetLastError=%d)\n", err);
            CloseHandle(hFile);
            return -1;
        }
#else
        static uint8_t *buf = NULL;
        if (!buf) {
            buf = (uint8_t*)malloc(CHUNK_SZ);
            if (!buf) { fclose(fp); return -1; }
        }

        size_t n = fread(buf, 1, chunk, fp);
        if (n != chunk) {
            pprintf("\n[system] fread failed\n");
            fclose(fp);
            return -1;
        }
        if (send_all(s, buf, (int)chunk) < 0) {
            fclose(fp);
            return -1;
        }
#endif

        offset += (uint64_t)chunk;
        sent += (uint64_t)chunk;

        ULONGLONG now = GetTickCount64();
        if (now - last_print >= PROGRESS_MS || sent == fsize) {
            double elapsed = (now - start_time) / 1000.0;
            double speed_bps = elapsed > 0 ? (double)sent / elapsed : 0.0;
            double speed_MB = speed_bps / (1024.0 * 1024.0);

            unsigned long long pct =
                (fsize == 0) ? 100ULL : (unsigned long long)((sent * 100ULL) / fsize);

            pprintf(
                "\r[send] %s: %llu%%  %.2f MB/s  (%llu/%llu bytes)",
                fname,
                pct,
                speed_MB,
                (unsigned long long)sent,
                (unsigned long long)fsize
            );

            last_print = now;
        }
    }

#ifdef _MSC_VER
    CloseHandle(hFile);
#else
    fclose(fp);
#endif

    double total_elapsed = (GetTickCount64() - start_time) / 1000.0;
    double avg_speed = total_elapsed > 0 ? (double)sent / total_elapsed : 0.0;

    pprintf(
        "\r[send] %s: 100%%  %.2f MB/s  (%llu/%llu bytes)\n",
        fname,
        avg_speed / (1024.0 * 1024.0),
        (unsigned long long)sent,
        (unsigned long long)fsize
    );

    return 0;
}

typedef struct {
    int active;
    FILE *fp;
    char outname[300];
    uint64_t total;
    uint64_t recvd;

    ULONGLONG last_print;
    ULONGLONG start_time;

    uint64_t last_recvd;
    ULONGLONG last_rate_t;
    double last_speed_bps;      // <<< 핵심: 마지막 속도 저장 (0.00 튐 방지)
} recv_file_state_t;

static int recv_file_begin(SOCKET s, uint32_t len, recv_file_state_t *st) {
    if (len < 2 + 8) return -1;

    uint16_t fn_net;
    if (recv_all(s, &fn_net, 2) < 0) return -1;
    uint16_t fn_len = ntohs(fn_net);

    if (len < (uint32_t)(2 + fn_len + 8)) return -1;
    if (fn_len >= 260) return -1;

    char fname[260];
    if (recv_all(s, fname, fn_len) < 0) return -1;
    fname[fn_len] = 0;

    uint64_t sz_net;
    if (recv_all(s, &sz_net, 8) < 0) return -1;
    uint64_t total = ntohll_u64(sz_net);

    char outname[300];
    make_recv_name(outname, sizeof(outname), fname);

    FILE *fp = fopen(outname, "wb");
    if (!fp) {
        pprintf("[system] Failed to create file: %s\n", outname);
        st->active = 0;
        st->fp = NULL;
        st->total = 0;
        st->recvd = 0;
        st->last_print = 0;
        st->start_time = 0;
        st->last_recvd = 0;
        st->last_rate_t = 0;
        st->last_speed_bps = 0.0;
        return 0;
    }

    setvbuf(fp, NULL, _IOFBF, FILE_IOBUF_SZ);

    st->active = 1;
    st->fp = fp;

    strncpy(st->outname, outname, sizeof(st->outname) - 1);
    st->outname[sizeof(st->outname) - 1] = 0;

    st->total = total;
    st->recvd = 0;

    st->last_print = 0;
    st->start_time = GetTickCount64();

    st->last_recvd = 0;
    st->last_rate_t = st->start_time;
    st->last_speed_bps = 0.0;

    pprintf("[system] Receiving file: %s (%llu bytes)\n", st->outname, (unsigned long long)st->total);

    if (st->total == 0) {
        fclose(st->fp);
        st->fp = NULL;
        st->active = 0;
        pprintf("\r[recv] %s: 100%%  0.00 MB/s  (0/0 bytes)\n", st->outname);
    }

    return 0;
}

static int recv_file_chunk(SOCKET s, uint32_t len, recv_file_state_t *st) {
    uint8_t buf_local[IO_BUF_SZ];

    uint32_t remain = len;
    while (remain > 0) {
        uint32_t chunk = remain > IO_BUF_SZ ? IO_BUF_SZ : remain;
        if (recv_all(s, buf_local, (int)chunk) < 0) return -1;

        if (st->active && st->fp) {
            fwrite(buf_local, 1, chunk, st->fp);
            st->recvd += (uint64_t)chunk;

            ULONGLONG now = GetTickCount64();

            if (now - st->last_print >= PROGRESS_MS || (st->total > 0 && st->recvd >= st->total)) {
                ULONGLONG dt = now - st->last_rate_t;

                // <<< 핵심: dt가 충분할 때만 last_speed 갱신, 아니면 이전 last_speed 그대로 출력
                if (dt >= RATE_UPDATE_MS) {
                    uint64_t dbytes = st->recvd - st->last_recvd;
                    st->last_speed_bps = (dt > 0)
                        ? ((double)dbytes / (dt / 1000.0))
                        : st->last_speed_bps;
                    st->last_recvd = st->recvd;
                    st->last_rate_t = now;
                }

                unsigned long long pct =
                    (st->total == 0) ? 100ULL : (unsigned long long)((st->recvd * 100ULL) / st->total);

                pprintf(
                    "\r[recv] %s: %llu%%  %.2f MB/s  (%llu/%llu bytes)",
                    st->outname,
                    pct,
                    st->last_speed_bps / (1024.0 * 1024.0),
                    (unsigned long long)st->recvd,
                    (unsigned long long)st->total
                );

                st->last_print = now;
            }

            if (st->total > 0 && st->recvd >= st->total) {
                double total_elapsed = (GetTickCount64() - st->start_time) / 1000.0;
                double avg_speed = total_elapsed > 0 ? (double)st->recvd / total_elapsed : 0.0;

                fclose(st->fp);
                st->fp = NULL;
                st->active = 0;

                pprintf(
                    "\r[recv] %s: 100%%  %.2f MB/s  (%llu/%llu bytes)\n",
                    st->outname,
                    avg_speed / (1024.0 * 1024.0),
                    (unsigned long long)st->recvd,
                    (unsigned long long)st->total
                );
            }
        }

        remain -= chunk;
    }

    return 0;
}

static DWORD WINAPI recv_loop(LPVOID arg) {
    SOCKET s = *(SOCKET *)arg;
    recv_file_state_t st;
    memset(&st, 0, sizeof(st));

    for (;;) {
        uint8_t type;
        uint32_t net_len;

        if (recv_all(s, &type, 1) < 0) break;
        if (recv_all(s, &net_len, 4) < 0) break;

        uint32_t len = ntohl(net_len);

        if (type == MSG_TEXT) {
            char *msg = (char*)malloc((size_t)len + 1);
            if (!msg) break;
            if (len > 0 && recv_all(s, msg, (int)len) < 0) { free(msg); break; }
            msg[len] = 0;
            pprintf("%s%s", msg, (len && msg[len - 1] == '\n') ? "" : "\n");
            free(msg);
        } else if (type == MSG_FILE_BEGIN) {
            if (recv_file_begin(s, len, &st) < 0) break;
        } else if (type == MSG_FILE_CHUNK) {
            if (recv_file_chunk(s, len, &st) < 0) break;
        } else {
            uint8_t dump[IO_BUF_SZ];
            uint32_t remain = len;
            while (remain > 0) {
                uint32_t chunk = remain > IO_BUF_SZ ? IO_BUF_SZ : remain;
                if (recv_all(s, dump, (int)chunk) < 0) goto end;
                remain -= chunk;
            }
        }
    }

end:
    if (st.fp) fclose(st.fp);
    pprintf("\n[system] Connection closed\n");
    return 0;
}

static void tune_socket(SOCKET s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    int sz = SOCKBUF_SZ;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&sz, sizeof(sz));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&sz, sizeof(sz));
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }

    InitializeCriticalSection(&g_print_cs);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        DeleteCriticalSection(&g_print_cs);
        return 1;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        fprintf(stderr, "Socket creation failed\n");
        WSACleanup();
        DeleteCriticalSection(&g_print_cs);
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IP address\n");
        closesocket(s);
        WSACleanup();
        DeleteCriticalSection(&g_print_cs);
        return 1;
    }

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "Connection failed\n");
        closesocket(s);
        WSACleanup();
        DeleteCriticalSection(&g_print_cs);
        return 1;
    }

    tune_socket(s);

    pprintf("[system] Connected\n");
    pprintf("Chat: type a message and press Enter\n");
    pprintf("File: /send <path>  (use quotes for spaces)\n");
    pprintf("Exit: Ctrl+Z then Enter\n\n");

    HANDLE t = CreateThread(NULL, 0, recv_loop, &s, 0, NULL);

    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        trim_newline(line);

        char path[2048];
        if (is_send_command(line, path, sizeof(path))) {
            if (send_file_chunked(s, path) < 0) {
                pprintf("\n[system] File send failed (WSA error=%d)\n", WSAGetLastError());
                break;
            }
        } else {
            if (send_text(s, line) < 0) {
                pprintf("\n[system] Send failed (WSA error=%d)\n", WSAGetLastError());
                break;
            }
        }
    }

    shutdown(s, SD_SEND);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);

    closesocket(s);
    WSACleanup();
    DeleteCriticalSection(&g_print_cs);
    return 0;
}

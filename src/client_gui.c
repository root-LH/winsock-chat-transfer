#define _CRT_SECURE_NO_WARNINGS
#include "client.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Comctl32.lib")

enum {
    IDC_IP = 101,
    IDC_PORT,
    IDC_ROOM,
    IDC_NICK,
    IDC_CONNECT,
    IDC_INPUT,
    IDC_SEND,
    IDC_SENDFILE,
    IDC_CANCELFILE,
    IDC_LOG,
    IDC_PB_SEND,
    IDC_PB_RECV,
    IDC_TX_SEND,
    IDC_TX_RECV
};

static HWND g_hwnd = NULL;
static HWND g_editLog = NULL;
static HWND g_pbSend = NULL;
static HWND g_pbRecv = NULL;
static HWND g_txSend = NULL;
static HWND g_txRecv = NULL;

static WNDPROC g_oldInputProc = NULL;

static SOCKET g_sock_chat = INVALID_SOCKET;
static SOCKET g_sock_file = INVALID_SOCKET;

static HANDLE g_thr_chat = NULL;
static HANDLE g_thr_file = NULL;

static volatile LONG g_connected = 0;
static volatile LONG g_disc_once = 0;

static HANDLE g_send_cancel_ev = NULL;
static volatile LONG g_send_inflight = 0;
static char g_send_cur_name[260] = {0};

static CRITICAL_SECTION g_chat_cs;
static CRITICAL_SECTION g_file_cs;

/* ---- offer wait (sender side) ---- */
static HANDLE g_ev_offer = NULL;                // manual-reset event
static volatile LONG g_offer_pending = 0;       // 1 if a send thread is waiting for accept/reject
static volatile LONG g_offer_result = 0;        // -1 pending, 0 reject, 1 accept

static void ui_append_log(const char *text) {
    if (!g_editLog) return;
    int len = GetWindowTextLengthA(g_editLog);
    SendMessageA(g_editLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(g_editLog, EM_REPLACESEL, 0, (LPARAM)text);
    SendMessageA(g_editLog, EM_SCROLLCARET, 0, 0);
}

static void post_logf(const char *fmt, ...) {
    char *buf = (char*)malloc(8192);
    if (!buf) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, 8192, fmt, ap);
    va_end(ap);
    PostMessageA(g_hwnd, WM_APP_LOG, 0, (LPARAM)buf);
}

static void post_state(int connected) {
    PostMessageA(g_hwnd, WM_APP_STATE, (WPARAM)connected, 0);
}

static void post_progress(int which, const char *name, uint64_t done, uint64_t total, double speed_bps, int done_flag) {
    progress_msg_t *pm = (progress_msg_t*)calloc(1, sizeof(*pm));
    if (!pm) return;
    pm->which = which;
    pm->done_bytes = done;
    pm->total_bytes = total;
    pm->speed_bps = speed_bps;
    pm->done = done_flag;
    if (name) {
        strncpy(pm->name, name, sizeof(pm->name)-1);
        pm->name[sizeof(pm->name)-1] = 0;
    }
    PostMessageA(g_hwnd, WM_APP_PROGRESS, 0, (LPARAM)pm);
}

static void sanitize_nick(char *s) {
    for (char *p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c <= 32 || c == 127) *p = '_';
    }
    if (s[0] == 0) strcpy(s, "user");
}

static void tune_socket(SOCKET s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    int sz = SOCKBUF_SZ;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&sz, sizeof(sz));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&sz, sizeof(sz));
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
    const char *p = (const char*)buf;
    int sent = 0;
    while (sent < len) {
        int n = send(s, p + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

static int recv_all(SOCKET s, void *buf, int len) {
    char *p = (char*)buf;
    int recvd = 0;
    while (recvd < len) {
        int n = recv(s, p + recvd, len - recvd, 0);
        if (n <= 0) return -1;
        recvd += n;
    }
    return recvd;
}

static int send_frame(SOCKET s, uint8_t type, const void *payload, uint32_t len) {
    uint32_t net_len = htonl(len);
    if (send_all(s, &type, 1) < 0) return -1;
    if (send_all(s, &net_len, 4) < 0) return -1;
    if (len > 0 && send_all(s, payload, (int)len) < 0) return -1;
    return 0;
}

static const char *basename_win(const char *path) {
    const char *p1 = strrchr(path, '\\');
    const char *p2 = strrchr(path, '/');
    const char *p = (p1 > p2) ? p1 : p2;
    return p ? p + 1 : path;
}

static void sanitize_filename_only(char *s) {
    for (char *p = s; *p; ++p) {
        if (strchr("<>:\"/\\|?*", *p)) *p = '_';
    }
    if (s[0] == 0) strcpy(s, "file");
}

static void make_recv_name(char *out, size_t out_sz, const char *orig) {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char *slash = strrchr(exePath, '\\');
    if (slash) *slash = '\0';

    char recvDir[MAX_PATH];
    snprintf(recvDir, sizeof(recvDir), "%s\\received", exePath);
    CreateDirectoryA(recvDir, NULL);

    char fname[260];
    strncpy(fname, orig, sizeof(fname)-1);
    fname[sizeof(fname)-1] = 0;
    sanitize_filename_only(fname);

    snprintf(out, out_sz, "%s\\recv_%s", recvDir, fname);
}

static LRESULT CALLBACK InputProc(HWND hEdit, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        send_chat(g_hwnd);
        return 0;
    }
    if (msg == WM_CHAR && wParam == '\r') {
        return 0;
    }
    return CallWindowProcA(g_oldInputProc, hEdit, msg, wParam, lParam);
}

static void close_both(void) {
    if (InterlockedCompareExchange(&g_disc_once, 1, 0) != 0) return;

    InterlockedExchange(&g_connected, 0);

    EnterCriticalSection(&g_chat_cs);
    if (g_sock_chat != INVALID_SOCKET) {
        shutdown(g_sock_chat, SD_BOTH);
        closesocket(g_sock_chat);
        g_sock_chat = INVALID_SOCKET;
    }
    LeaveCriticalSection(&g_chat_cs);

    EnterCriticalSection(&g_file_cs);
    if (g_sock_file != INVALID_SOCKET) {
        shutdown(g_sock_file, SD_BOTH);
        closesocket(g_sock_file);
        g_sock_file = INVALID_SOCKET;
    }
    LeaveCriticalSection(&g_file_cs);

    // wake sender waiting offer
    if (g_ev_offer) SetEvent(g_ev_offer);

    post_state(0);
    post_logf("[system] Disconnected\r\n");
}

static SOCKET connect_role(const char *ip, const char *port, const char *room, const char *kind, const char *nick) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)atoi(port));
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        closesocket(s);
        return INVALID_SOCKET;
    }

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }

    tune_socket(s);

    char nn[NICK_MAX];
    strncpy(nn, nick, NICK_MAX-1);
    nn[NICK_MAX-1] = 0;
    sanitize_nick(nn);

    char hello[256];
    snprintf(hello, sizeof(hello), "ROLE %s %s %s\n", kind, room, nn);
    if (send_all(s, hello, (int)strlen(hello)) < 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }

    return s;
}

/* ---------- recv threads ---------- */

static DWORD WINAPI recv_chat_loop(LPVOID unused) {
    (void)unused;
    for (;;) {
        SOCKET s;
        EnterCriticalSection(&g_chat_cs);
        s = g_sock_chat;
        LeaveCriticalSection(&g_chat_cs);
        if (s == INVALID_SOCKET) break;

        uint8_t type;
        uint32_t net_len;
        if (recv_all(s, &type, 1) < 0) break;
        if (recv_all(s, &net_len, 4) < 0) break;
        uint32_t len = ntohl(net_len);

        if (type != MSG_TEXT) {
            char dump[IO_BUF_SZ];
            uint32_t remain = len;
            while (remain) {
                uint32_t c = remain > IO_BUF_SZ ? IO_BUF_SZ : remain;
                if (recv_all(s, dump, (int)c) < 0) goto end;
                remain -= c;
            }
            continue;
        }

        char *msg = (char*)malloc((size_t)len + 1);
        if (!msg) break;
        if (len && recv_all(s, msg, (int)len) < 0) { free(msg); break; }
        msg[len] = 0;

        post_logf("%s\r\n", msg);
        free(msg);
    }
end:
    close_both();
    return 0;
}

/* ---- receiver: OFFER handling ---- */
static int recv_file_begin_offer(SOCKET s, uint32_t len, recv_file_state_t *st) {
    if (len < 2 + 8) return -1;

    uint16_t fn_net;
    if (recv_all(s, &fn_net, 2) < 0) return -1;
    uint16_t fn_len = ntohs(fn_net);
    if (len < (uint32_t)(2 + fn_len + 8) || fn_len >= 260) return -1;

    char fname[260];
    if (recv_all(s, fname, fn_len) < 0) return -1;
    fname[fn_len] = 0;

    uint64_t sz_net;
    if (recv_all(s, &sz_net, 8) < 0) return -1;
    uint64_t total = ntohll_u64(sz_net);

    // Ask user accept/reject
    char prompt[512];
    double mb = (double)total / (1024.0 * 1024.0);
    snprintf(prompt, sizeof(prompt),
             "Incoming file:\r\n\r\n  %s\r\n  Size: %.2f MB\r\n\r\nAccept?",
             fname, mb);

    int ans = MessageBoxA(g_hwnd, prompt, "File Transfer Request",
                          MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON1);

    if (ans != IDYES) {
        // reject
        if (send_frame(s, MSG_FILE_REJECT, NULL, 0) < 0) return -1;

        st->active = 0;
        st->discard = 1;
        st->fp = NULL;
        st->total = total;
        st->recvd = 0;
        st->last_speed_bps = 0.0;

        post_logf("[system] Rejected file: %s\r\n", fname);
        post_progress(1, "(rejected)", 0, total, 0.0, 1);
        return 0;
    }

    // accept
    if (send_frame(s, MSG_FILE_ACCEPT, NULL, 0) < 0) return -1;

    char outname[300];
    make_recv_name(outname, sizeof(outname), fname);

    FILE *fp = fopen(outname, "wb");
    if (!fp) {
        post_logf("[system] Failed to create file: %s (discard)\r\n", outname);
        st->active = 0;
        st->discard = 1;
        st->fp = NULL;
        st->total = total;
        st->recvd = 0;
        st->last_speed_bps = 0.0;
        post_progress(1, "(discard)", 0, total, 0.0, 0);
        return 0;
    }

    st->active = 1;
    st->discard = 0;
    st->fp = fp;
    strncpy(st->outname, outname, sizeof(st->outname)-1);
    st->outname[sizeof(st->outname)-1] = 0;
    st->total = total;
    st->recvd = 0;
    st->last_ui = 0;
    st->start_time = GetTickCount64();
    st->last_recvd = 0;
    st->last_rate_t = st->start_time;
    st->last_speed_bps = 0.0;

    post_logf("[system] Receiving file: %s\r\n", st->outname);
    post_progress(1, st->outname, 0, st->total, 0.0, 0);
    return 0;
}

static int recv_file_chunk(SOCKET s, uint32_t len, recv_file_state_t *st) {
    char buf[IO_BUF_SZ];
    uint32_t remain = len;

    while (remain) {
        uint32_t c = remain > IO_BUF_SZ ? IO_BUF_SZ : remain;
        if (recv_all(s, buf, (int)c) < 0) return -1;

        if (st->active && st->fp && !st->discard) fwrite(buf, 1, c, st->fp);
        st->recvd += (uint64_t)c;

        ULONGLONG now = GetTickCount64();
        if (now - st->last_ui >= PROGRESS_MS || (st->total && st->recvd >= st->total)) {
            ULONGLONG dt = now - st->last_rate_t;
            if (dt >= RATE_UPDATE_MS) {
                uint64_t dbytes = st->recvd - st->last_recvd;
                st->last_speed_bps = (dt > 0) ? ((double)dbytes / (dt / 1000.0)) : st->last_speed_bps;
                st->last_recvd = st->recvd;
                st->last_rate_t = now;
            }
            post_progress(1,
                          st->discard ? "(discard)" : (st->outname[0] ? st->outname : "-"),
                          st->recvd, st->total, st->last_speed_bps,
                          (st->total && st->recvd >= st->total));
            st->last_ui = now;
        }

        if (!st->discard && st->active && st->fp && st->total && st->recvd >= st->total) {
            fclose(st->fp);
            st->fp = NULL;
            st->active = 0;
            post_progress(1, st->outname, st->total, st->total, st->last_speed_bps, 1);
            post_logf("[system] File received: %s\r\n", st->outname);
        }

        remain -= c;
    }
    return 0;
}

static DWORD WINAPI recv_file_loop(LPVOID unused) {
    (void)unused;
    recv_file_state_t st;
    memset(&st, 0, sizeof(st));

    for (;;) {
        SOCKET s;
        EnterCriticalSection(&g_file_cs);
        s = g_sock_file;
        LeaveCriticalSection(&g_file_cs);
        if (s == INVALID_SOCKET) break;

        uint8_t type;
        uint32_t net_len;
        if (recv_all(s, &type, 1) < 0) break;
        if (recv_all(s, &net_len, 4) < 0) break;
        uint32_t len = ntohl(net_len);

        if (type == MSG_FILE_BEGIN) {
            // OFFER
            if (recv_file_begin_offer(s, len, &st) < 0) break;

        } else if (type == MSG_FILE_CHUNK) {
            if (recv_file_chunk(s, len, &st) < 0) break;

        } else if (type == MSG_FILE_ACCEPT || type == MSG_FILE_REJECT) {
            // sender side: wake up sending thread that is waiting for response
            // payload should be empty; drain anyway if not
            if (len > 0) {
                char dump[IO_BUF_SZ];
                uint32_t remain = len;
                while (remain) {
                    uint32_t c = remain > IO_BUF_SZ ? IO_BUF_SZ : remain;
                    if (recv_all(s, dump, (int)c) < 0) goto end;
                    remain -= c;
                }
            }

            if (InterlockedCompareExchange(&g_offer_pending, 1, 1) == 1) {
                InterlockedExchange(&g_offer_result, (type == MSG_FILE_ACCEPT) ? 1 : 0);
                if (g_ev_offer) SetEvent(g_ev_offer);
            }

        } else if (type == MSG_FILE_CANCEL){
            uint16_t fn_net = 0;
            uint16_t fn_len = 0;
            char namebuf[260] = {0};

            if (len >= 2) {
                if (recv_all(s, &fn_net, 2) < 0) break;
                fn_len = ntohs(fn_net);
                if (fn_len > 0 && fn_len < 260 && (uint32_t)(2 + fn_len) <= len) {
                    if (recv_all(s, namebuf, fn_len) < 0) break;
                    namebuf[fn_len] = 0;
                } else {
                    uint32_t remain = len - 2;
                    char dump[IO_BUF_SZ];
                    while (remain) {
                        uint32_t c = remain > IO_BUF_SZ ? IO_BUF_SZ : remain;
                        if (recv_all(s, dump, (int)c) < 0) goto end;
                        remain -= c;
                    }
                }
            }
        } else if (type == MSG_TEXT) {
            char *msg = (char*)malloc((size_t)len + 1);
            if (!msg) break;
            if (len && recv_all(s, msg, (int)len) < 0) { free(msg); break; }
            msg[len] = 0;
            post_logf("%s\r\n", msg);
            free(msg);

        } else {
            char dump[IO_BUF_SZ];
            uint32_t remain = len;
            while (remain) {
                uint32_t c = remain > IO_BUF_SZ ? IO_BUF_SZ : remain;
                if (recv_all(s, dump, (int)c) < 0) goto end;
                remain -= c;
            }
        }
    }

end:
    close_both();
    return 0;
}

/* ---------- send helpers ---------- */

static int send_text_chat_raw(const char *text) {
    EnterCriticalSection(&g_chat_cs);
    SOCKET s = g_sock_chat;
    LeaveCriticalSection(&g_chat_cs);
    if (s == INVALID_SOCKET) return -1;
    return send_frame(s, MSG_TEXT, text, (uint32_t)strlen(text));
}

static DWORD WINAPI send_file_thread(LPVOID lp) {
    char *path = (char*)lp;

    // only one outgoing file offer at a time (simple)
    if (InterlockedCompareExchange(&g_offer_pending, 1, 0) != 0) {
        post_logf("[system] Another file send is already pending.\r\n");
        free(path);
        return 0;
    }

    ResetEvent(g_send_cancel_ev);
    InterlockedExchange(&g_send_inflight, 1);
    strncpy(g_send_cur_name, basename_win(path), sizeof(g_send_cur_name)-1);
    g_send_cur_name[sizeof(g_send_cur_name)-1] = 0;

    PostMessageA(g_hwnd, WM_APP_STATE, 1, 0);

    EnterCriticalSection(&g_file_cs);
    SOCKET s = g_sock_file;
    LeaveCriticalSection(&g_file_cs);
    if (s == INVALID_SOCKET) {
        InterlockedExchange(&g_offer_pending, 0);
        free(path);
        return 0;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        post_logf("[system] Failed to open file: %s\r\n", path);
        InterlockedExchange(&g_offer_pending, 0);
        free(path);
        return 0;
    }

    _fseeki64(fp, 0, SEEK_END);
    uint64_t fsize = (uint64_t)_ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);

    const char *fname = basename_win(path);
    uint16_t fname_len = (uint16_t)strlen(fname);

    uint32_t begin_len = (uint32_t)(2u + fname_len + 8u);
    uint8_t *begin_payload = (uint8_t*)malloc(begin_len);
    if (!begin_payload) {
        fclose(fp);
        InterlockedExchange(&g_offer_pending, 0);
        free(path);
        return 0;
    }

    uint16_t net_fn = htons(fname_len);
    uint64_t net_sz = htonll_u64(fsize);

    memcpy(begin_payload + 0, &net_fn, 2);
    memcpy(begin_payload + 2, fname, fname_len);
    memcpy(begin_payload + 2 + fname_len, &net_sz, 8);

    // reset offer wait
    InterlockedExchange(&g_offer_result, -1);
    if (g_ev_offer) ResetEvent(g_ev_offer);

    // send OFFER (MSG_FILE_BEGIN)
    if (send_frame(s, MSG_FILE_BEGIN, begin_payload, begin_len) < 0) {
        free(begin_payload); fclose(fp); free(path);
        InterlockedExchange(&g_offer_pending, 0);
        close_both();
        return 0;
    }
    free(begin_payload);

    post_logf("[system] File offer sent. Waiting accept...\r\n");

    // wait accept/reject (up to 120 seconds)
    DWORD wr = WaitForSingleObject(g_ev_offer, 120000);
    if (wr != WAIT_OBJECT_0) {
        post_logf("[system] No response (timeout). Cancel.\r\n");
        fclose(fp);
        InterlockedExchange(&g_offer_pending, 0);
        free(path);
        return 0;
    }

    LONG res = InterlockedCompareExchange(&g_offer_result, g_offer_result, g_offer_result);
    if (res != 1) {
        post_logf("[system] Receiver rejected the file.\r\n");
        fclose(fp);
        InterlockedExchange(&g_offer_pending, 0);
        free(path);
        return 0;
    }

    post_logf("[system] Receiver accepted. Sending...\r\n");

    uint8_t *buf = (uint8_t*)malloc(FILE_CHUNK_SZ);
    if (!buf) {
        fclose(fp);
        InterlockedExchange(&g_offer_pending, 0);
        free(path);
        return 0;
    }

    uint64_t sent = 0;
    ULONGLONG start = GetTickCount64();
    ULONGLONG last_ui = 0;

    post_progress(0, fname, 0, fsize, 0.0, 0);

    while (sent < fsize) {
        if (WaitForSingleObject(g_send_cancel_ev, 0) == WAIT_OBJECT_0) {
            send_file_cancel_notify(fname);

            post_logf("[system] File send canceled: %s\r\n", fname);
            post_progress(0, fname, sent, fsize, 0.0, 1);

            free(buf);
            fclose(fp);
            free(path);

            InterlockedExchange(&g_send_inflight, 0);
            g_send_cur_name[0] = 0;
            InterlockedExchange(&g_offer_pending, 0);
            return 0;
        }

        size_t n = fread(buf, 1, FILE_CHUNK_SZ, fp);
        if (n == 0) break;

        if (send_frame(s, MSG_FILE_CHUNK, buf, (uint32_t)n) < 0) {
            free(buf); fclose(fp); free(path);
            InterlockedExchange(&g_offer_pending, 0);
            close_both();
            return 0;
        }

        sent += (uint64_t)n;

        ULONGLONG now = GetTickCount64();
        if (now - last_ui >= PROGRESS_MS || sent >= fsize) {
            double elapsed = (now - start) / 1000.0;
            double speed = elapsed > 0 ? (double)sent / elapsed : 0.0;
            post_progress(0, fname, sent, fsize, speed, (sent >= fsize));
            last_ui = now;
        }
    }

    free(buf);
    fclose(fp);

    post_progress(0, fname, fsize, fsize, 0.0, 1);
    post_logf("[system] File send finished: %s\r\n", fname);

    InterlockedExchange(&g_send_inflight, 0);
    g_send_cur_name[0] = 0;
    InterlockedExchange(&g_offer_pending, 0);
    free(path);
    return 0;
}

/* ---------- GUI ---------- */

static void update_ui_connected(HWND hWnd, int connected) {
    SetWindowTextA(GetDlgItem(hWnd, IDC_CONNECT), connected ? "Disconnect" : "Connect");
    EnableWindow(GetDlgItem(hWnd, IDC_SEND), connected);
    EnableWindow(GetDlgItem(hWnd, IDC_SENDFILE), connected);
    EnableWindow(GetDlgItem(hWnd, IDC_CANCELFILE), connected);
}

static void ui_apply_progress(progress_msg_t *pm) {
    HWND pb = (pm->which == 0) ? g_pbSend : g_pbRecv;
    HWND tx = (pm->which == 0) ? g_txSend : g_txRecv;
    if (!pb || !tx) return;

    uint64_t total = pm->total_bytes;
    uint64_t done  = pm->done_bytes;

    int pos = 0;
    if (total == 0) pos = 10000;
    else {
        long double pct = (long double)done * 10000.0L / (long double)total;
        if (pct < 0) pct = 0;
        if (pct > 10000) pct = 10000;
        pos = (int)pct;
    }
    SendMessageA(pb, PBM_SETPOS, (WPARAM)pos, 0);

    char line[512];
    double doneMB  = (double)done / (1024.0 * 1024.0);
    double totalMB = (double)total / (1024.0 * 1024.0);
    double speedMB = pm->speed_bps / (1024.0 * 1024.0);
    double pct2 = (total == 0) ? 100.0 : ((double)done * 100.0 / (double)total);

    snprintf(line, sizeof(line),
             "%s  %.2f%%  (%.2f / %.2f MB)  %.2f MB/s%s",
             (pm->name[0] ? pm->name : "-"),
             pct2, doneMB, totalMB, speedMB,
             pm->done ? "  [done]" : "");
    SetWindowTextA(tx, line);
}

static HWND create_progress(HWND parent, int x, int y, int w, int h, int id) {
    return CreateWindowExA(0, PROGRESS_CLASSA, "", WS_CHILD | WS_VISIBLE,
                           x, y, w, h, parent, (HMENU)(INT_PTR)id, NULL, NULL);
}

static void create_controls(HWND hWnd) {
    CreateWindowA("STATIC", "IP:", WS_CHILD | WS_VISIBLE, 10, 10, 20, 20, hWnd, NULL, NULL, NULL);
    CreateWindowA("EDIT", "127.0.0.1", WS_CHILD | WS_VISIBLE | WS_BORDER, 35, 8, 140, 22, hWnd, (HMENU)IDC_IP, NULL, NULL);

    CreateWindowA("STATIC", "Port:", WS_CHILD | WS_VISIBLE, 185, 10, 35, 20, hWnd, NULL, NULL, NULL);
    CreateWindowA("EDIT", "9000", WS_CHILD | WS_VISIBLE | WS_BORDER, 225, 8, 60, 22, hWnd, (HMENU)IDC_PORT, NULL, NULL);

    CreateWindowA("STATIC", "Room:", WS_CHILD | WS_VISIBLE, 295, 10, 40, 20, hWnd, NULL, NULL, NULL);
    CreateWindowA("EDIT", "default", WS_CHILD | WS_VISIBLE | WS_BORDER, 340, 8, 85, 22, hWnd, (HMENU)IDC_ROOM, NULL, NULL);

    CreateWindowA("STATIC", "Nick:", WS_CHILD | WS_VISIBLE, 435, 10, 35, 20, hWnd, NULL, NULL, NULL);
    CreateWindowA("EDIT", "user", WS_CHILD | WS_VISIBLE | WS_BORDER, 475, 8, 90, 22, hWnd, (HMENU)IDC_NICK, NULL, NULL);

    CreateWindowA("BUTTON", "Connect", WS_CHILD | WS_VISIBLE, 575, 7, 120, 24, hWnd, (HMENU)IDC_CONNECT, NULL, NULL);

    HWND hInput = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                  10, 40, 555, 24, hWnd, (HMENU)IDC_INPUT, NULL, NULL);
    g_oldInputProc = (WNDPROC)(LONG_PTR)SetWindowLongPtrA(hInput, GWLP_WNDPROC, (LONG_PTR)InputProc);

    CreateWindowA("BUTTON", "Send", WS_CHILD | WS_VISIBLE, 575, 40, 120, 24, hWnd, (HMENU)IDC_SEND, NULL, NULL);
    CreateWindowA("BUTTON", "Send File", WS_CHILD | WS_VISIBLE, 575, 70, 120, 24, hWnd, (HMENU)IDC_SENDFILE, NULL, NULL);

    CreateWindowA("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE, 575, 110, 120, 24, hWnd, (HMENU)IDC_CANCELFILE, NULL, NULL);
    EnableWindow(GetDlgItem(hWnd, IDC_CANCELFILE), FALSE);

    CreateWindowA("STATIC", "Send:", WS_CHILD | WS_VISIBLE, 10, 74, 45, 18, hWnd, NULL, NULL, NULL);
    g_pbSend = create_progress(hWnd, 60, 72, 505, 18, IDC_PB_SEND);
    g_txSend = CreateWindowA("STATIC", "-", WS_CHILD | WS_VISIBLE, 10, 92, 700, 18, hWnd, (HMENU)IDC_TX_SEND, NULL, NULL);

    CreateWindowA("STATIC", "Recv:", WS_CHILD | WS_VISIBLE, 10, 114, 45, 18, hWnd, NULL, NULL, NULL);
    g_pbRecv = create_progress(hWnd, 60, 112, 505, 18, IDC_PB_RECV);
    g_txRecv = CreateWindowA("STATIC", "-", WS_CHILD | WS_VISIBLE, 10, 132, 700, 18, hWnd, (HMENU)IDC_TX_RECV, NULL, NULL);

    SendMessageA(g_pbSend, PBM_SETRANGE32, 0, 10000);
    SendMessageA(g_pbRecv, PBM_SETRANGE32, 0, 10000);

    g_editLog = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER |
                              ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY,
                              10, 160, 685, 360, hWnd, (HMENU)IDC_LOG, NULL, NULL);

    update_ui_connected(hWnd, 0);
}

static void layout_controls(HWND hWnd, int w, int h) {
    int margin = 10;
    int btnW = 120;

    int topRightX = w - margin - btnW;
    int inputW = topRightX - margin - 10;
    if (inputW < 80) inputW = 80;

    int pbW = topRightX - margin - 60;
    if (pbW < 80) pbW = 80;

    int logH = h - 170;
    if (logH < 60) logH = 60;

    HDWP dwp = BeginDeferWindowPos(32);
    if (!dwp) return;

    dwp = DeferWindowPos(dwp, GetDlgItem(hWnd, IDC_CONNECT), NULL, topRightX, 7, btnW, 24, SWP_NOZORDER);

    dwp = DeferWindowPos(dwp, GetDlgItem(hWnd, IDC_INPUT), NULL, margin, 40, inputW, 24, SWP_NOZORDER);
    dwp = DeferWindowPos(dwp, GetDlgItem(hWnd, IDC_SEND), NULL, topRightX, 40, btnW, 24, SWP_NOZORDER);
    dwp = DeferWindowPos(dwp, GetDlgItem(hWnd, IDC_SENDFILE), NULL, topRightX, 70, btnW, 24, SWP_NOZORDER);
    dwp = DeferWindowPos(dwp, GetDlgItem(hWnd, IDC_CANCELFILE), NULL, topRightX, 110, btnW, 24, SWP_NOZORDER);
    
    dwp = DeferWindowPos(dwp, g_pbSend, NULL, 60, 72, pbW, 18, SWP_NOZORDER);
    dwp = DeferWindowPos(dwp, g_txSend, NULL, margin, 92, w - margin*2, 18, SWP_NOZORDER);

    dwp = DeferWindowPos(dwp, g_pbRecv, NULL, 60, 112, pbW, 18, SWP_NOZORDER);
    dwp = DeferWindowPos(dwp, g_txRecv, NULL, margin, 132, w - margin*2, 18, SWP_NOZORDER);

    dwp = DeferWindowPos(dwp, g_editLog, NULL, margin, 160, w - margin*2, logH, SWP_NOZORDER);

    EndDeferWindowPos(dwp);
    RedrawWindow(hWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static void pick_and_send_file(HWND hWnd) {
    if (!InterlockedCompareExchange(&g_connected, 1, 1)) return;

    char path[MAX_PATH] = {0};
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = "All Files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameA(&ofn)) return;

    char *dup = _strdup(path);
    if (!dup) return;

    CreateThread(NULL, 0, send_file_thread, dup, 0, NULL);
    post_logf("[system] File send requested\r\n");
}

static void send_chat(HWND hWnd) {
    if (!InterlockedCompareExchange(&g_connected, 1, 1)) return;

    char msg[2048];
    GetWindowTextA(GetDlgItem(hWnd, IDC_INPUT), msg, (int)sizeof(msg));
    if (msg[0] == 0) return;

    char nick[NICK_MAX];
    GetWindowTextA(GetDlgItem(hWnd, IDC_NICK), nick, (int)sizeof(nick));
    nick[NICK_MAX-1] = 0;
    sanitize_nick(nick);

    char out[2300];
    _snprintf(out, sizeof(out), "%s: %s", nick, msg);

    if (send_text_chat_raw(out) < 0) {
        close_both();
        return;
    }

    post_logf("[me] %s\r\n", out);
    SetWindowTextA(GetDlgItem(hWnd, IDC_INPUT), "");
}

static int do_connect(HWND hWnd) {
    char ip[128], port[32], room[64], nick[NICK_MAX];
    GetWindowTextA(GetDlgItem(hWnd, IDC_IP), ip, sizeof(ip));
    GetWindowTextA(GetDlgItem(hWnd, IDC_PORT), port, sizeof(port));
    GetWindowTextA(GetDlgItem(hWnd, IDC_ROOM), room, sizeof(room));
    GetWindowTextA(GetDlgItem(hWnd, IDC_NICK), nick, sizeof(nick));
    if (room[0] == 0) strcpy(room, "default");
    sanitize_nick(nick);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        post_logf("[system] WSAStartup failed\r\n");
        return -1;
    }

    SOCKET s_chat = connect_role(ip, port, room, "CHAT", nick);
    if (s_chat == INVALID_SOCKET) {
        post_logf("[system] Connect CHAT failed\r\n");
        WSACleanup();
        return -1;
    }

    SOCKET s_file = connect_role(ip, port, room, "FILE", nick);
    if (s_file == INVALID_SOCKET) {
        post_logf("[system] Connect FILE failed\r\n");
        closesocket(s_chat);
        WSACleanup();
        return -1;
    }

    InterlockedExchange(&g_disc_once, 0);

    EnterCriticalSection(&g_chat_cs);
    g_sock_chat = s_chat;
    LeaveCriticalSection(&g_chat_cs);

    EnterCriticalSection(&g_file_cs);
    g_sock_file = s_file;
    LeaveCriticalSection(&g_file_cs);

    InterlockedExchange(&g_connected, 1);

    g_thr_chat = CreateThread(NULL, 0, recv_chat_loop, NULL, 0, NULL);
    g_thr_file = CreateThread(NULL, 0, recv_file_loop, NULL, 0, NULL);

    post_logf("[system] Connected (CHAT+FILE)\r\n");
    post_state(1);
    return 0;
}

static void do_disconnect(void) {
    close_both();

    if (g_thr_chat) { WaitForSingleObject(g_thr_chat, INFINITE); CloseHandle(g_thr_chat); g_thr_chat = NULL; }
    if (g_thr_file) { WaitForSingleObject(g_thr_file, INFINITE); CloseHandle(g_thr_file); g_thr_file = NULL; }

    WSACleanup();
}

/* ---------- WndProc ---------- */

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        create_controls(hWnd);
        return 0;

    case WM_SIZE:
        layout_controls(hWnd, LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_APP_LOG: {
        char *p = (char*)lParam;
        ui_append_log(p);
        free(p);
        return 0;
    }

    case WM_APP_STATE:
        update_ui_connected(hWnd, (int)wParam);
        return 0;

    case WM_APP_PROGRESS: {
        progress_msg_t *pm = (progress_msg_t*)lParam;
        ui_apply_progress(pm);
        free(pm);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
            case IDC_CONNECT:
                if (!InterlockedCompareExchange(&g_connected, 1, 1)) do_connect(hWnd);
                else do_disconnect();
                return 0;

            case IDC_SEND:
                send_chat(hWnd);
                return 0;

            case IDC_SENDFILE:
                pick_and_send_file(hWnd);
                return 0;
                
            case IDC_CANCELFILE:
                if (InterlockedCompareExchange(&g_send_inflight, 1, 1)) {
                    SetEvent(g_send_cancel_ev);
                    post_logf("[system] Cancel requested...\r\n");
                }
                return 0;
        }
        break;

    case WM_CLOSE:
        if (InterlockedCompareExchange(&g_connected, 1, 1)) do_disconnect();
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

static void send_file_cancel_notify(const char *fname) {
    EnterCriticalSection(&g_file_cs);
    SOCKET s = g_sock_file;
    LeaveCriticalSection(&g_file_cs);
    if (s == INVALID_SOCKET) return;

    // payload: uint16 fname_len + fname bytes (간단)
    uint16_t fn_len = (uint16_t)strlen(fname);
    if (fn_len > 250) fn_len = 250;

    uint16_t net_fn = htons(fn_len);
    uint8_t payload[2 + 260];
    memcpy(payload, &net_fn, 2);
    memcpy(payload + 2, fname, fn_len);

    send_frame(s, MSG_FILE_CANCEL, payload, 2u + fn_len);
}


int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    InitializeCriticalSection(&g_chat_cs);
    InitializeCriticalSection(&g_file_cs);

    // offer wait event (manual-reset)
    g_ev_offer = CreateEventA(NULL, TRUE, FALSE, NULL);

    g_send_cancel_ev = CreateEventA(NULL, TRUE, FALSE, NULL);

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "ChatClientGUI_2Sock_Nick";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassA(&wc)) return 1;

    HWND hWnd = CreateWindowExA(
        WS_EX_COMPOSITED,
        "ChatClientGUI_2Sock_Nick",
        "1:1 Chat + File Transfer (2 sockets, nickname)",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT, 740, 560,
        NULL, NULL, hInst, NULL
    );
    if (!hWnd) return 1;

    g_hwnd = hWnd;

    ShowWindow(hWnd, nShow);
    UpdateWindow(hWnd);

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }

    if (g_ev_offer) { CloseHandle(g_ev_offer); g_ev_offer = NULL; }
    if (g_send_cancel_ev) CloseHandle(g_send_cancel_ev);

    DeleteCriticalSection(&g_chat_cs);
    DeleteCriticalSection(&g_file_cs);
    return 0;
}

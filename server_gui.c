#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Comctl32.lib")

// must match client protocol
#define MSG_TEXT       1
#define MSG_FILE_BEGIN 2
#define MSG_FILE_CHUNK 3

#define MAX_ROOMS 64
#define ROOM_NAME_MAX 64
#define NICK_MAX 32
#define LINE_MAX 256
#define RELAY_BUF_SZ (256 * 1024)

#define WM_APP_LOG (WM_APP + 1)

enum { IDC_PORT = 101, IDC_LISTEN, IDC_LOG };

typedef struct {
    char name[ROOM_NAME_MAX];
    SOCKET chat[2];
    SOCKET file[2];
    char chat_nick[2][NICK_MAX];
    char file_nick[2][NICK_MAX];
    int used;
} room_t;

typedef struct { SOCKET a, b; } relay_arg_t;

static HWND g_hwnd = NULL;
static HWND g_editLog = NULL;

static CRITICAL_SECTION g_cs;
static room_t g_rooms[MAX_ROOMS];

static SOCKET g_ls = INVALID_SOCKET;
static HANDLE g_accept_thr = NULL;
static volatile LONG g_listening = 0;

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
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, 8192, fmt, ap);
    va_end(ap);
    PostMessageA(g_hwnd, WM_APP_LOG, 0, (LPARAM)buf);
}

static void set_listen_ui(HWND hWnd, int on) {
    SetWindowTextA(GetDlgItem(hWnd, IDC_LISTEN), on ? "Stop" : "Listen");
    EnableWindow(GetDlgItem(hWnd, IDC_PORT), !on);
}

static void closesock_safe(SOCKET *s) {
    if (*s != INVALID_SOCKET) {
        shutdown(*s, SD_BOTH);
        closesocket(*s);
        *s = INVALID_SOCKET;
    }
}

static int recv_line(SOCKET s, char *out, int out_sz) {
    int n = 0;
    while (n + 1 < out_sz) {
        char c;
        int r = recv(s, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') break;
        if (c == '\r') continue;
        out[n++] = c;
    }
    out[n] = 0;
    return n;
}

static room_t* get_room_locked(const char *name) {
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (g_rooms[i].used && strcmp(g_rooms[i].name, name) == 0) return &g_rooms[i];
    }
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (!g_rooms[i].used) {
            g_rooms[i].used = 1;
            strncpy(g_rooms[i].name, name, ROOM_NAME_MAX - 1);
            g_rooms[i].name[ROOM_NAME_MAX - 1] = 0;
            g_rooms[i].chat[0] = g_rooms[i].chat[1] = INVALID_SOCKET;
            g_rooms[i].file[0] = g_rooms[i].file[1] = INVALID_SOCKET;
            g_rooms[i].chat_nick[0][0] = g_rooms[i].chat_nick[1][0] = 0;
            g_rooms[i].file_nick[0][0] = g_rooms[i].file_nick[1][0] = 0;
            return &g_rooms[i];
        }
    }
    return NULL;
}

static int pick_slot(SOCKET arr[2]) {
    if (arr[0] == INVALID_SOCKET) return 0;
    if (arr[1] == INVALID_SOCKET) return 1;
    return -1;
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

// server -> client notify using client's framing
static int send_text_frame(SOCKET s, const char *text) {
    uint8_t type = MSG_TEXT;
    uint32_t len = (uint32_t)strlen(text);
    uint32_t net_len = htonl(len);
    if (send_all(s, &type, 1) < 0) return -1;
    if (send_all(s, &net_len, 4) < 0) return -1;
    if (len && send_all(s, text, (int)len) < 0) return -1;
    return 0;
}

static DWORD WINAPI relay_thread(LPVOID p) {
    relay_arg_t *arg = (relay_arg_t*)p;
    SOCKET a = arg->a, b = arg->b;
    free(arg);

    char *buf = (char*)malloc(RELAY_BUF_SZ);
    if (!buf) {
        shutdown(a, SD_BOTH); shutdown(b, SD_BOTH);
        closesocket(a); closesocket(b);
        return 0;
    }

    for (;;) {
        int r = recv(a, buf, RELAY_BUF_SZ, 0);
        if (r <= 0) break;
        int off = 0;
        while (off < r) {
            int s = send(b, buf + off, r - off, 0);
            if (s <= 0) { r = -1; break; }
            off += s;
        }
        if (r < 0) break;
    }

    free(buf);
    shutdown(a, SD_BOTH); shutdown(b, SD_BOTH);
    closesocket(a); closesocket(b);
    return 0;
}

static void start_relay_pair_with_nick(SOCKET x, SOCKET y, const char *tag,
                                       const char *room, const char *nick_x, const char *nick_y) {
    post_logf("[room %s] %s paired (%s <-> %s)\r\n", room, tag,
              (nick_x && nick_x[0]) ? nick_x : "?", (nick_y && nick_y[0]) ? nick_y : "?");

    // notify each side who is the peer
    {
        char mx[256], my[256];
        snprintf(mx, sizeof(mx), "[system] %s peer connected: %s (room=%s)", tag,
                 (nick_y && nick_y[0]) ? nick_y : "unknown", room);
        snprintf(my, sizeof(my), "[system] %s peer connected: %s (room=%s)", tag,
                 (nick_x && nick_x[0]) ? nick_x : "unknown", room);
        send_text_frame(x, mx);
        send_text_frame(y, my);
    }

    relay_arg_t *ab = (relay_arg_t*)calloc(1, sizeof(*ab));
    relay_arg_t *ba = (relay_arg_t*)calloc(1, sizeof(*ba));
    if (!ab || !ba) {
        closesocket(x); closesocket(y);
        if (ab) free(ab);
        if (ba) free(ba);
        return;
    }
    ab->a = x; ab->b = y;
    ba->a = y; ba->b = x;

    CreateThread(NULL, 0, relay_thread, ab, 0, NULL);
    CreateThread(NULL, 0, relay_thread, ba, 0, NULL);
}

static void try_start_room_locked(room_t *rm) {
    if (rm->chat[0] != INVALID_SOCKET && rm->chat[1] != INVALID_SOCKET) {
        SOCKET a = rm->chat[0], b = rm->chat[1];
        char na[NICK_MAX], nb[NICK_MAX];
        strncpy(na, rm->chat_nick[0], NICK_MAX); na[NICK_MAX-1]=0;
        strncpy(nb, rm->chat_nick[1], NICK_MAX); nb[NICK_MAX-1]=0;

        rm->chat[0] = rm->chat[1] = INVALID_SOCKET;
        rm->chat_nick[0][0] = rm->chat_nick[1][0] = 0;

        start_relay_pair_with_nick(a, b, "CHAT", rm->name, na, nb);
    }
    if (rm->file[0] != INVALID_SOCKET && rm->file[1] != INVALID_SOCKET) {
        SOCKET a = rm->file[0], b = rm->file[1];
        char na[NICK_MAX], nb[NICK_MAX];
        strncpy(na, rm->file_nick[0], NICK_MAX); na[NICK_MAX-1]=0;
        strncpy(nb, rm->file_nick[1], NICK_MAX); nb[NICK_MAX-1]=0;

        rm->file[0] = rm->file[1] = INVALID_SOCKET;
        rm->file_nick[0][0] = rm->file_nick[1][0] = 0;

        start_relay_pair_with_nick(a, b, "FILE", rm->name, na, nb);
    }
}

static DWORD WINAPI accept_worker(LPVOID unused) {
    (void)unused;

    for (;;) {
        if (!InterlockedCompareExchange(&g_listening, 1, 1)) break;

        struct sockaddr_in cli;
        int clen = sizeof(cli);
        SOCKET s = accept(g_ls, (struct sockaddr*)&cli, &clen);
        if (s == INVALID_SOCKET) {
            if (!InterlockedCompareExchange(&g_listening, 1, 1)) break;
            Sleep(10);
            continue;
        }

        char line[LINE_MAX];
        if (recv_line(s, line, sizeof(line)) < 0) {
            closesocket(s);
            continue;
        }

        // ROLE <CHAT|FILE> <room> <nick>
        char role[16], kind[16], room[ROOM_NAME_MAX], nick[NICK_MAX];
        role[0]=kind[0]=room[0]=nick[0]=0;
        sscanf(line, "%15s %15s %63s %31s", role, kind, room, nick);

        if (_stricmp(role, "ROLE") != 0 || room[0] == 0 || nick[0] == 0 ||
            !(_stricmp(kind, "CHAT")==0 || _stricmp(kind, "FILE")==0)) {
            const char *msg = "ERR\n";
            send(s, msg, (int)strlen(msg), 0);
            closesocket(s);
            continue;
        }

        EnterCriticalSection(&g_cs);
        room_t *rm = get_room_locked(room);
        if (!rm) {
            LeaveCriticalSection(&g_cs);
            closesocket(s);
            continue;
        }

        SOCKET *arr = (_stricmp(kind, "CHAT")==0) ? rm->chat : rm->file;
        char (*narr)[NICK_MAX] = (_stricmp(kind, "CHAT")==0) ? rm->chat_nick : rm->file_nick;

        int slot = pick_slot(arr);
        if (slot < 0) {
            LeaveCriticalSection(&g_cs);
            const char *msg = "BUSY\n";
            send(s, msg, (int)strlen(msg), 0);
            closesocket(s);
            post_logf("[room %s] %s rejected (full)\r\n", room, kind);
            continue;
        }

        arr[slot] = s;
        strncpy(narr[slot], nick, NICK_MAX-1);
        narr[slot][NICK_MAX-1] = 0;

        post_logf("[room %s] %s joined slot=%d nick=%s\r\n", room, kind, slot, nick);

        try_start_room_locked(rm);
        LeaveCriticalSection(&g_cs);
    }
    return 0;
}

static int server_start(HWND hWnd, int port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        post_logf("[system] WSAStartup failed\r\n");
        return -1;
    }

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) {
        post_logf("[system] socket failed (WSA=%d)\r\n", WSAGetLastError());
        WSACleanup();
        return -1;
    }

    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(ls, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        post_logf("[system] bind failed (WSA=%d)\r\n", WSAGetLastError());
        closesocket(ls);
        WSACleanup();
        return -1;
    }
    if (listen(ls, 64) == SOCKET_ERROR) {
        post_logf("[system] listen failed (WSA=%d)\r\n", WSAGetLastError());
        closesocket(ls);
        WSACleanup();
        return -1;
    }

    EnterCriticalSection(&g_cs);
    for (int i=0;i<MAX_ROOMS;i++){
        g_rooms[i].used = 0;
        g_rooms[i].chat[0]=g_rooms[i].chat[1]=INVALID_SOCKET;
        g_rooms[i].file[0]=g_rooms[i].file[1]=INVALID_SOCKET;
        g_rooms[i].chat_nick[0][0]=g_rooms[i].chat_nick[1][0]=0;
        g_rooms[i].file_nick[0][0]=g_rooms[i].file_nick[1][0]=0;
    }
    LeaveCriticalSection(&g_cs);

    g_ls = ls;
    InterlockedExchange(&g_listening, 1);
    g_accept_thr = CreateThread(NULL, 0, accept_worker, NULL, 0, NULL);

    post_logf("[system] Listening on port %d\r\n", port);
    set_listen_ui(hWnd, 1);
    return 0;
}

static void server_stop(HWND hWnd) {
    if (!InterlockedCompareExchange(&g_listening, 0, 1)) return;

    closesock_safe(&g_ls);

    if (g_accept_thr) {
        WaitForSingleObject(g_accept_thr, INFINITE);
        CloseHandle(g_accept_thr);
        g_accept_thr = NULL;
    }

    EnterCriticalSection(&g_cs);
    for (int i=0;i<MAX_ROOMS;i++){
        if (!g_rooms[i].used) continue;
        for (int k=0;k<2;k++){
            if (g_rooms[i].chat[k] != INVALID_SOCKET) closesocket(g_rooms[i].chat[k]);
            if (g_rooms[i].file[k] != INVALID_SOCKET) closesocket(g_rooms[i].file[k]);
            g_rooms[i].chat[k] = INVALID_SOCKET;
            g_rooms[i].file[k] = INVALID_SOCKET;
        }
        g_rooms[i].used = 0;
    }
    LeaveCriticalSection(&g_cs);

    WSACleanup();
    post_logf("[system] Stopped\r\n");
    set_listen_ui(hWnd, 0);
}

static void create_controls(HWND hWnd) {
    CreateWindowA("STATIC", "Port:", WS_CHILD|WS_VISIBLE, 10, 12, 40, 20, hWnd, NULL, NULL, NULL);
    CreateWindowA("EDIT", "9000", WS_CHILD|WS_VISIBLE|WS_BORDER|ES_NUMBER, 55, 10, 80, 22, hWnd, (HMENU)IDC_PORT, NULL, NULL);
    CreateWindowA("BUTTON", "Listen", WS_CHILD|WS_VISIBLE, 145, 9, 90, 24, hWnd, (HMENU)IDC_LISTEN, NULL, NULL);

    g_editLog = CreateWindowA("EDIT", "", WS_CHILD|WS_VISIBLE|WS_BORDER|
                              ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL|ES_READONLY,
                              10, 45, 560, 360, hWnd, (HMENU)IDC_LOG, NULL, NULL);
}

static void layout_controls(HWND hWnd, int w, int h) {
    int margin = 10;
    int logY = 45;

    HDWP dwp = BeginDeferWindowPos(3);
    if (!dwp) return;

    dwp = DeferWindowPos(dwp, GetDlgItem(hWnd, IDC_PORT), NULL, 55, 10, 80, 22, SWP_NOZORDER);
    dwp = DeferWindowPos(dwp, GetDlgItem(hWnd, IDC_LISTEN), NULL, 145, 9, 90, 24, SWP_NOZORDER);
    dwp = DeferWindowPos(dwp, g_editLog, NULL, margin, logY, w - margin*2, h - logY - margin, SWP_NOZORDER);

    EndDeferWindowPos(dwp);
    RedrawWindow(hWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        create_controls(hWnd);
        set_listen_ui(hWnd, 0);
        return 0;

    case WM_SIZE:
        layout_controls(hWnd, LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_LISTEN) {
            if (!InterlockedCompareExchange(&g_listening, 1, 1)) {
                char portStr[32];
                GetWindowTextA(GetDlgItem(hWnd, IDC_PORT), portStr, sizeof(portStr));
                int port = atoi(portStr);
                if (port <= 0 || port > 65535) {
                    post_logf("[system] Invalid port\r\n");
                    return 0;
                }
                server_start(hWnd, port);
            } else {
                server_stop(hWnd);
            }
            return 0;
        }
        break;

    case WM_APP_LOG: {
        char *p = (char*)lParam;
        ui_append_log(p);
        free(p);
        return 0;
    }

    case WM_CLOSE:
        server_stop(hWnd);
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    InitializeCriticalSection(&g_cs);

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "RelayServerGUI_Nick";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassA(&wc)) return 1;

    HWND hWnd = CreateWindowExA(
        WS_EX_COMPOSITED,
        "RelayServerGUI_Nick",
        "Relay Server (GUI) - Nickname",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT, 620, 460,
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

    DeleteCriticalSection(&g_cs);
    return 0;
}

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

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

typedef struct {
    char name[ROOM_NAME_MAX];
    SOCKET chat[2];
    SOCKET file[2];
    char chat_nick[2][NICK_MAX];
    char file_nick[2][NICK_MAX];
    int used;
} room_t;

typedef struct { SOCKET a, b; } relay_arg_t;

static void ui_append_log(const char *text);
static void post_logf(const char *fmt, ...);
static void set_listen_ui(HWND hWnd, int on);
static void closesock_safe(SOCKET *s);
static int recv_line(SOCKET s, char *out, int out_sz);
static room_t* get_room_locked(const char *name);
static int pick_slot(SOCKET arr[2]);
static int send_all(SOCKET s, const void *buf, int len);
static int send_text_frame(SOCKET s, const char *text);
static DWORD WINAPI relay_thread(LPVOID p);
static void start_relay_pair_with_nick(SOCKET x, SOCKET y, const char *tag,
                                       const char *room, const char *nick_x, const char *nick_y);
static void try_start_room_locked(room_t *rm);
static DWORD WINAPI accept_worker(LPVOID unused);
static int server_start(HWND hWnd, int port);
static void server_stop(HWND hWnd);
static void create_controls(HWND hWnd);
static void layout_controls(HWND hWnd, int w, int h);
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow);
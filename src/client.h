#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#define MSG_TEXT        1
#define MSG_FILE_BEGIN  2   // now: OFFER (fname+size)
#define MSG_FILE_CHUNK  3
#define MSG_FILE_ACCEPT 4
#define MSG_FILE_REJECT 5

#define IO_BUF_SZ      (256 * 1024)
#define FILE_CHUNK_SZ  (256 * 1024)
#define SOCKBUF_SZ     (4 * 1024 * 1024)

#define PROGRESS_MS    200
#define RATE_UPDATE_MS 500

#define WM_APP_LOG       (WM_APP + 1)
#define WM_APP_STATE     (WM_APP + 2)
#define WM_APP_PROGRESS  (WM_APP + 3)

#define NICK_MAX 32

typedef struct {
    int which;            // 0=send, 1=recv
    uint64_t done_bytes;
    uint64_t total_bytes;
    double speed_bps;
    char name[260];
    int done;
} progress_msg_t;

typedef struct {
    int active;     // actively writing file
    int discard;    // receiver rejected; discard any chunks if they arrive anyway
    FILE *fp;
    char outname[300];
    uint64_t total;
    uint64_t recvd;

    ULONGLONG last_ui;
    ULONGLONG start_time;

    uint64_t last_recvd;
    ULONGLONG last_rate_t;
    double last_speed_bps;
} recv_file_state_t;

static void ui_append_log(const char *text);
static void post_logf(const char *fmt, ...);
static void post_state(int connected);
static void post_progress(int which, const char *name, uint64_t done, uint64_t total, double speed_bps, int done_flag);
static void sanitize_nick(char *s);
static void tune_socket(SOCKET s);
static uint64_t htonll_u64(uint64_t x);
static uint64_t ntohll_u64(uint64_t x);
static int send_all(SOCKET s, const void *buf, int len);
static int recv_all(SOCKET s, void *buf, int len);
static int send_frame(SOCKET s, uint8_t type, const void *payload, uint32_t len);
static const char *basename_win(const char *path);
static void sanitize_filename_only(char *s);
static void make_recv_name(char *out, size_t out_sz, const char *orig);
static LRESULT CALLBACK InputProc(HWND hEdit, UINT msg, WPARAM wParam, LPARAM lParam);
static void close_both(void);
static SOCKET connect_role(const char *ip, const char *port, const char *room, const char *kind, const char *nick);
static DWORD WINAPI recv_chat_loop(LPVOID unused);
static int recv_file_begin_offer(SOCKET s, uint32_t len, recv_file_state_t *st);
static int recv_file_chunk(SOCKET s, uint32_t len, recv_file_state_t *st);
static DWORD WINAPI recv_file_loop(LPVOID unused);
static int send_text_chat_raw(const char *text);
static DWORD WINAPI send_file_thread(LPVOID lp);
static void update_ui_connected(HWND hWnd, int connected);
static void ui_apply_progress(progress_msg_t *pm);
static HWND create_progress(HWND parent, int x, int y, int w, int h, int id);
static void create_controls(HWND hWnd);
static void layout_controls(HWND hWnd, int w, int h);
static void pick_and_send_file(HWND hWnd);
static void send_chat(HWND hWnd);
static int do_connect(HWND hWnd);
static void do_disconnect(void);
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow);
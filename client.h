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

static LRESULT CALLBACK InputProc(HWND hEdit, UINT msg, WPARAM wParam, LPARAM lParam);
static void send_chat(HWND hWnd);
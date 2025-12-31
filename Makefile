CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -std=c11

LIBS_COMMON := -lws2_32 -lcomctl32 -mwindows

LIBS_CLIENT := -lcomdlg32
LIBS_SERVER := 

CLIENT_EXE := client.exe
SERVER_EXE := server.exe

COMMON_SRC := 
CLIENT_SRC := client_gui.c $(COMMON_SRC)
SERVER_SRC := server_gui.c $(COMMON_SRC)

CLIENT_OBJ := $(CLIENT_SRC:.c=.o)
SERVER_OBJ := $(SERVER_SRC:.c=.o)

.PHONY: all client server clean

all: client server

client: $(CLIENT_EXE)
server: $(SERVER_EXE)

$(CLIENT_EXE): $(CLIENT_OBJ)
	$(CC) $^ -o $@ $(LIBS_COMMON) $(LIBS_CLIENT)

$(SERVER_EXE): $(SERVER_OBJ)
	$(CC) $^ -o $@ $(LIBS_COMMON) $(LIBS_SERVER)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	del /Q *.o *.exe 2>nul || true
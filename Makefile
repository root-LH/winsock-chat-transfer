CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -std=c11

SRC_DIR		:= src
BUILD_DIR	:= build

LIBS_COMMON := -lws2_32 -lcomctl32 -mwindows

LIBS_CLIENT := -lcomdlg32
LIBS_SERVER := 

CLIENT_EXE := $(BUILD_DIR)/client.exe
SERVER_EXE := $(BUILD_DIR)/server.exe

COMMON_SRC := 
CLIENT_SRC := client_gui.c $(COMMON_SRC)
SERVER_SRC := server_gui.c $(COMMON_SRC)

CLIENT_OBJ := $(addprefix $(BUILD_DIR)/,$(CLIENT_SRC:.c=.o))
SERVER_OBJ := $(addprefix $(BUILD_DIR)/,$(SERVER_SRC:.c=.o))

.PHONY: all client server clean

all: client server

client: $(CLIENT_EXE)
server: $(SERVER_EXE)

$(BUILD_DIR):
	mkdir $(BUILD_DIR)

$(CLIENT_EXE): $(BUILD_DIR) $(CLIENT_OBJ)
	$(CC) $(CLIENT_OBJ) -o $@ $(LIBS_COMMON) $(LIBS_CLIENT)

$(SERVER_EXE): $(BUILD_DIR) $(SERVER_OBJ)
	$(CC) $(SERVER_OBJ) -o $@ $(LIBS_COMMON) $(LIBS_SERVER)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	del /Q $(BUILD_DIR)\*.o $(BUILD_DIR)\*.exe 2>nul || true
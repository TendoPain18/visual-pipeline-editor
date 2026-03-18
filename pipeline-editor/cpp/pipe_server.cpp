#include <windows.h>
#include <stdio.h>
#include <signal.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <time.h>

#pragma comment(lib, "ws2_32.lib")

#define MAX_BUFFERS 100
#define SOCKET_PORT 9000
#define MAX_MSG_SIZE 4096

HANDLE handles[MAX_BUFFERS * 3];
volatile bool running = true;
SOCKET clientSocket = INVALID_SOCKET;

// Configuration
int numBuffers = 0;
unsigned long long bufferSizes[MAX_BUFFERS];
char pipeNames[MAX_BUFFERS][128];

void cleanup(int sig) {
    printf("\nShutting down gracefully...\n");
    running = false;
}

// Send JSON message to Electron
bool sendMessage(SOCKET sock, const char* type, const char* message, const char* data = NULL) {
    if (sock == INVALID_SOCKET) return false;
    
    char jsonMsg[MAX_MSG_SIZE];
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char timestamp[26];
    strftime(timestamp, 26, "%Y-%m-%d %H:%M:%S", tm_info);
    
    if (data) {
        snprintf(jsonMsg, sizeof(jsonMsg), 
            "{\"type\":\"%s\",\"message\":\"%s\",\"data\":\"%s\",\"timestamp\":\"%s\"}\n",
            type, message, data, timestamp);
    } else {
        snprintf(jsonMsg, sizeof(jsonMsg), 
            "{\"type\":\"%s\",\"message\":\"%s\",\"timestamp\":\"%s\"}\n",
            type, message, timestamp);
    }
    
    int sent = send(sock, jsonMsg, strlen(jsonMsg), 0);
    if (sent == SOCKET_ERROR) {
        printf("Send failed: %d\n", WSAGetLastError());
        return false;
    }
    return true;
}

// Check if client is still connected
bool isClientConnected(SOCKET sock) {
    if (sock == INVALID_SOCKET) return false;
    
    char buffer;
    int result = recv(sock, &buffer, 1, MSG_PEEK);
    
    if (result == 0) {
        // Connection closed gracefully
        return false;
    } else if (result == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
            // No data available, but connection is alive
            return true;
        }
        // Other errors mean connection is dead
        return false;
    }
    
    return true;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, cleanup);
    
    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("ERROR: WSAStartup failed\n");
        return 1;
    }
    
    printf("========================================\n");
    printf("PIPE SERVER - Socket Communication\n");
    printf("========================================\n");
    printf("Version:     2.0-Socket\n");
    printf("Socket Port: %d\n", SOCKET_PORT);
    printf("========================================\n\n");
    
    // Parse command line arguments
    if (argc < 2) {
        printf("ERROR: Usage: pipe_server.exe <num_buffers> <pipe1> <size1> <pipe2> <size2> ...\n");
        WSACleanup();
        return 1;
    }
    
    numBuffers = atoi(argv[1]);
    
    if (numBuffers <= 0 || numBuffers > MAX_BUFFERS) {
        printf("ERROR: Invalid number of buffers: %d\n", numBuffers);
        WSACleanup();
        return 1;
    }
    
    // Parse pipe names and sizes
    int argIndex = 2;
    for (int i = 0; i < numBuffers; i++) {
        if (argIndex >= argc) {
            printf("ERROR: Missing pipe name for buffer %d\n", i);
            WSACleanup();
            return 1;
        }
        strncpy(pipeNames[i], argv[argIndex++], sizeof(pipeNames[i]) - 1);
        
        if (argIndex >= argc) {
            printf("ERROR: Missing size for buffer %d\n", i);
            WSACleanup();
            return 1;
        }
        bufferSizes[i] = strtoull(argv[argIndex++], NULL, 10);
    }
    
    printf("Configuration loaded: %d buffers\n\n", numBuffers);
    
    // Create socket
    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        printf("ERROR: Socket creation failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }
    
    // Set socket to non-blocking mode for connection checks
    u_long mode = 1;
    ioctlsocket(serverSocket, FIONBIO, &mode);
    
    // Bind socket
    struct sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(SOCKET_PORT);
    
    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        printf("ERROR: Bind failed: %d\n", WSAGetLastError());
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }
    
    // Listen for connections
    if (listen(serverSocket, 1) == SOCKET_ERROR) {
        printf("ERROR: Listen failed: %d\n", WSAGetLastError());
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }
    
    printf("Socket server listening on port %d...\n", SOCKET_PORT);
    printf("Waiting for Electron client to connect...\n\n");
    
    // Wait for client connection
    struct sockaddr_in clientAddr;
    int clientAddrSize = sizeof(clientAddr);
    
    while (clientSocket == INVALID_SOCKET && running) {
        clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientAddrSize);
        if (clientSocket == INVALID_SOCKET) {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK) {
                printf("ERROR: Accept failed: %d\n", error);
                break;
            }
            Sleep(100); // Wait a bit before trying again
        }
    }
    
    if (clientSocket == INVALID_SOCKET) {
        printf("ERROR: Failed to accept client connection\n");
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }
    
    // Set client socket to non-blocking for connection monitoring
    u_long clientMode = 1;
    ioctlsocket(clientSocket, FIONBIO, &clientMode);
    
    printf("Client connected from %s:%d\n\n", 
           inet_ntoa(clientAddr.sin_addr), 
           ntohs(clientAddr.sin_port));
    
    // Send connection confirmation
    sendMessage(clientSocket, "CONNECTED", "Server connected to client");
    
    // Create shared memory and events
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;
    
    sendMessage(clientSocket, "STATUS", "Creating shared memory buffers...");
    
    int idx = 0;
    for (int i = 0; i < numBuffers; i++) {
        // Create file mapping
        handles[idx] = CreateFileMappingA(
            INVALID_HANDLE_VALUE, 
            &sa, 
            PAGE_READWRITE,
            (DWORD)(bufferSizes[i] >> 32),
            (DWORD)(bufferSizes[i] & 0xFFFFFFFF),
            pipeNames[i]
        );
        
        if (!handles[idx]) {
            char errMsg[256];
            snprintf(errMsg, sizeof(errMsg), "Failed to create %s", pipeNames[i]);
            sendMessage(clientSocket, "ERROR", errMsg);
            printf("ERROR: %s (Error: %d)\n", errMsg, GetLastError());
            running = false;
            break;
        }
        
        char successMsg[256];
        snprintf(successMsg, sizeof(successMsg), "Created %s (%.2f MB)", 
                 pipeNames[i], bufferSizes[i] / (1024.0 * 1024.0));
        sendMessage(clientSocket, "PIPE_CREATED", successMsg, pipeNames[i]);
        printf("%s\n", successMsg);
        idx++;
        
        // Create events
        char readyName[256], emptyName[256];
        sprintf(readyName, "%s_Ready", pipeNames[i]);
        sprintf(emptyName, "%s_Empty", pipeNames[i]);
        
        handles[idx++] = CreateEventA(&sa, FALSE, FALSE, readyName);
        handles[idx++] = CreateEventA(&sa, FALSE, TRUE, emptyName);
    }
    
    if (running) {
        printf("\n========================================\n");
        printf("PIPELINE SYSTEM READY\n");
        printf("========================================\n");
        printf("Active Buffers: %d\n", numBuffers);
        printf("Socket: Connected\n");
        printf("Monitoring client connection...\n");
        printf("Press Ctrl+C to shutdown\n");
        printf("========================================\n\n");
        
        sendMessage(clientSocket, "READY", "Pipeline system ready", NULL);
        
        // Main loop - monitor connection
        int checkCounter = 0;
        while (running) {
            // Check if client is still connected
            if (!isClientConnected(clientSocket)) {
                printf("\n========================================\n");
                printf("CLIENT DISCONNECTED\n");
                printf("========================================\n");
                printf("Electron client has disconnected.\n");
                printf("Initiating shutdown...\n");
                running = false;
                break;
            }
            
            // Heartbeat every 10 seconds
            checkCounter++;
            if (checkCounter >= 100) { // 100 * 100ms = 10 seconds
                sendMessage(clientSocket, "HEARTBEAT", "Server alive");
                checkCounter = 0;
            }
            
            Sleep(100); // Check every 100ms
        }
    }
    
    // Cleanup
    printf("\n========================================\n");
    printf("CLEANUP\n");
    printf("========================================\n");
    
    sendMessage(clientSocket, "SHUTDOWN", "Server shutting down");
    
    printf("Closing handles...\n");
    for (int i = 0; i < idx; i++) {
        if (handles[i]) {
            CloseHandle(handles[i]);
        }
    }
    
    printf("Closing sockets...\n");
    if (clientSocket != INVALID_SOCKET) {
        closesocket(clientSocket);
    }
    closesocket(serverSocket);
    
    WSACleanup();
    
    printf("========================================\n");
    printf("Server shutdown complete\n");
    printf("========================================\n");
    
    return 0;
}

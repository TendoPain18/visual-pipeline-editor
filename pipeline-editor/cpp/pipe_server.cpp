#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <signal.h>
#include <string>
#include <vector>
#include <set>
#include <map>

#pragma comment(lib, "ws2_32.lib")

// SOCKET-BASED PIPE SERVER
// Accepts port as command-line argument for multi-instance support
// FEATURE: Tracks MATLAB block PIDs and kills them if UI socket disconnects

std::vector<HANDLE> handles;
volatile bool running = true;
SOCKET serverSocket = INVALID_SOCKET;
SOCKET clientSocket = INVALID_SOCKET;

// PID registry: pid -> block name (for logging)
std::map<DWORD, std::string> registeredPids;

void cleanup(int sig) {
    printf("\nShutting down...\n");
    running = false;
}

void sendMessage(SOCKET sock, const char* type, const char* message) {
    if (sock == INVALID_SOCKET) return;

    char buffer[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);

    snprintf(buffer, sizeof(buffer),
        "{\"type\":\"%s\",\"message\":\"%s\",\"timestamp\":\"%02d:%02d:%02d.%03d\"}\n",
        type, message, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    send(sock, buffer, (int)strlen(buffer), 0);
}

// Kill a process by PID using TerminateProcess
void killPid(DWORD pid, const char* name) {
    HANDLE hProc = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProc) {
        printf("[PID Cleanup] Cannot open process %lu (%s) - may already be gone\n", pid, name);
        return;
    }

    // Check if it's still alive before killing
    DWORD exitCode = 0;
    if (GetExitCodeProcess(hProc, &exitCode) && exitCode == STILL_ACTIVE) {
        if (TerminateProcess(hProc, 1)) {
            printf("[PID Cleanup] Killed PID %lu (%s)\n", pid, name);
        } else {
            printf("[PID Cleanup] Failed to kill PID %lu (%s) - Error: %lu\n", pid, name, GetLastError());
        }
    } else {
        printf("[PID Cleanup] PID %lu (%s) already exited (code: %lu)\n", pid, name, exitCode);
    }

    CloseHandle(hProc);
}

// Kill all registered MATLAB block processes
void killAllRegisteredPids() {
    if (registeredPids.empty()) {
        printf("[PID Cleanup] No registered PIDs to clean up\n");
        return;
    }

    printf("\n========================================\n");
    printf("[PID Cleanup] Killing %zu registered MATLAB process(es)...\n", registeredPids.size());
    printf("========================================\n");

    for (auto& entry : registeredPids) {
        killPid(entry.first, entry.second.c_str());
    }

    registeredPids.clear();
    printf("[PID Cleanup] Done.\n\n");
}

// Parse a simple JSON field: {"type":"REGISTER_PID","pid":1234,"name":"FileSource"}
// We only need a minimal parser for these control messages.
bool extractJsonString(const std::string& json, const std::string& key, std::string& outVal) {
    // Look for "key":"value"
    std::string searchKey = "\"" + key + "\":\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return false;
    pos += searchKey.size();
    size_t end = json.find("\"", pos);
    if (end == std::string::npos) return false;
    outVal = json.substr(pos, end - pos);
    return true;
}

bool extractJsonNumber(const std::string& json, const std::string& key, long& outVal) {
    // Look for "key":number
    std::string searchKey = "\"" + key + "\":";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return false;
    pos += searchKey.size();
    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size()) return false;
    char* endPtr = nullptr;
    outVal = strtol(json.c_str() + pos, &endPtr, 10);
    return (endPtr != json.c_str() + pos);
}

// Handle a control message received from Electron
void handleControlMessage(const std::string& json) {
    std::string msgType;
    if (!extractJsonString(json, "type", msgType)) return;

    if (msgType == "REGISTER_PID") {
        long pid = 0;
        std::string name = "unknown";
        extractJsonNumber(json, "pid", pid);
        extractJsonString(json, "name", name);

        if (pid > 0) {
            registeredPids[(DWORD)pid] = name;
            printf("[PID Registry] Registered PID %ld (%s) | Total tracked: %zu\n",
                   pid, name.c_str(), registeredPids.size());
        }

    } else if (msgType == "UNREGISTER_PID") {
        long pid = 0;
        std::string name = "unknown";
        extractJsonNumber(json, "pid", pid);
        extractJsonString(json, "name", name);

        if (pid > 0) {
            auto it = registeredPids.find((DWORD)pid);
            if (it != registeredPids.end()) {
                registeredPids.erase(it);
                printf("[PID Registry] Unregistered PID %ld (%s) | Remaining tracked: %zu\n",
                       pid, name.c_str(), registeredPids.size());
            }
        }

    } else if (msgType == "PING") {
        // UI is alive - nothing to do, heartbeat handled via recv check
    }
}

int main(int argc, char* argv[]) {
    signal(SIGINT, cleanup);

    if (argc < 3) {
        printf("Usage: pipe_server.exe <num_buffers> <port> <pipe1_name> <size1> ...\n");
        printf("Example: pipe_server.exe 2 9000 Instance_12345_P1 67108864 Instance_12345_P2 67108864\n");
        return 1;
    }

    int numBuffers = atoi(argv[1]);
    int port = atoi(argv[2]);

    if (numBuffers <= 0 || port <= 0) {
        printf("ERROR: Invalid number of buffers or port\n");
        return 1;
    }

    int expectedArgs = 3 + (numBuffers * 2);
    if (argc != expectedArgs) {
        printf("ERROR: Expected %d arguments, got %d\n", expectedArgs, argc);
        return 1;
    }

    printf("========================================\n");
    printf("PIPE SERVER - Socket Version\n");
    printf("========================================\n");
    printf("Port:        %d\n", port);
    printf("Num Buffers: %d\n", numBuffers);
    printf("PID Tracking: ENABLED\n");
    printf("========================================\n\n");

    // Initialize Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        printf("ERROR: WSAStartup failed: %d\n", result);
        return 1;
    }

    // Create socket server
    serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        printf("ERROR: Socket creation failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    int reuse = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        printf("ERROR: Bind failed on port %d: %d\n", port, WSAGetLastError());
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    if (listen(serverSocket, 1) == SOCKET_ERROR) {
        printf("ERROR: Listen failed: %d\n", WSAGetLastError());
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    printf("Socket server listening on port %d\n\n", port);

    // Create shared memory pipes
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    for (int i = 0; i < numBuffers; i++) {
        int argIdx = 3 + (i * 2);
        const char* pipeName = argv[argIdx];
        unsigned long long size = strtoull(argv[argIdx + 1], NULL, 10);

        HANDLE hFile = CreateFileMappingA(
            INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
            (DWORD)(size >> 32),
            (DWORD)(size & 0xFFFFFFFF),
            pipeName
        );

        if (!hFile) {
            printf("ERROR: Failed to create %s (Error: %lu)\n", pipeName, GetLastError());
            return 1;
        }

        handles.push_back(hFile);
        printf("Created: %s (%.2f MB)\n", pipeName, size / (1024.0 * 1024.0));

        char rName[256], eName[256];
        sprintf(rName, "%s_Ready", pipeName);
        sprintf(eName, "%s_Empty", pipeName);
        handles.push_back(CreateEventA(&sa, FALSE, FALSE, rName));
        handles.push_back(CreateEventA(&sa, FALSE, TRUE, eName));
    }

    printf("\nPIPE SYSTEM: Initialized (%d buffers)\n", numBuffers);
    printf("Waiting for Electron connection...\n\n");

    // Accept connection (blocking)
    clientSocket = accept(serverSocket, NULL, NULL);
    if (clientSocket == INVALID_SOCKET) {
        printf("ERROR: Accept failed: %d\n", WSAGetLastError());
    } else {
        printf("✓ Electron connected via socket\n\n");
        sendMessage(clientSocket, "CONNECTED", "Pipe server ready");
        sendMessage(clientSocket, "READY", "All pipes created successfully");

        // Set socket to non-blocking for the recv check
        u_long mode = 1;
        ioctlsocket(clientSocket, FIONBIO, &mode);

        std::string recvBuffer;
        DWORD lastHeartbeat = GetTickCount();

        while (running) {
            Sleep(200); // Check 5x per second

            // Try to receive any incoming control messages
            char buf[4096];
            int recvResult = recv(clientSocket, buf, sizeof(buf) - 1, 0);

            if (recvResult > 0) {
                buf[recvResult] = '\0';
                recvBuffer += buf;

                // Process complete newline-delimited messages
                size_t newlinePos;
                while ((newlinePos = recvBuffer.find('\n')) != std::string::npos) {
                    std::string line = recvBuffer.substr(0, newlinePos);
                    recvBuffer = recvBuffer.substr(newlinePos + 1);

                    if (!line.empty() && line[0] == '{') {
                        handleControlMessage(line);
                    }
                }

                lastHeartbeat = GetTickCount();

            } else if (recvResult == 0) {
                // Clean disconnect
                printf("\n========================================\n");
                printf("Electron disconnected (clean close)\n");
                printf("========================================\n");
                running = false;

            } else {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK) {
                    // No data right now - normal, continue heartbeat check
                    DWORD now = GetTickCount();
                    if (now - lastHeartbeat > 5000) {
                        sendMessage(clientSocket, "HEARTBEAT", "Server alive");
                        lastHeartbeat = now;
                    }
                } else if (err == WSAECONNRESET || err == WSAECONNABORTED || err == WSAENETDOWN) {
                    // Abnormal disconnect - UI likely crashed
                    printf("\n========================================\n");
                    printf("SOCKET ERROR: UI may have crashed! (WSA error: %d)\n", err);
                    printf("========================================\n");
                    running = false;
                }
            }
        }

        // --- Socket closed (either clean or crash) ---
        // Kill all registered MATLAB PIDs before we exit
        killAllRegisteredPids();

        sendMessage(clientSocket, "SHUTDOWN", "Server shutting down");
        closesocket(clientSocket);
    }

    printf("\nCleaning up shared memory...\n");
    for (HANDLE h : handles) {
        if (h) CloseHandle(h);
    }

    closesocket(serverSocket);
    WSACleanup();

    printf("Shutdown complete\n");
    return 0;
}
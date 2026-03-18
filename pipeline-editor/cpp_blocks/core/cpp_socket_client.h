#ifndef CPP_SOCKET_CLIENT_H
#define CPP_SOCKET_CLIENT_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <ctime>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "ws2_32.lib")

class CppSocketClient {
private:
    SOCKET sock;
    bool connected;
    std::string host;
    int port;

    std::string getCurrentTimestamp() {
        auto now = std::time(nullptr);
        auto tm = *std::localtime(&now);
        std::ostringstream oss;
        oss << std::put_time(&tm, "%H:%M:%S");
        return oss.str();
    }

public:
    CppSocketClient(const std::string& host = "127.0.0.1", int port = 9002) 
        : sock(INVALID_SOCKET), connected(false), host(host), port(port) {
        
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            fprintf(stderr, "[SOCKET] WSAStartup failed: %d\n", result);
            return;
        }
    }

    ~CppSocketClient() {
        if (connected) {
            closesocket(sock);
        }
        WSACleanup();
    }

    bool connect(int maxRetries = 10) {
        for (int attempt = 1; attempt <= maxRetries; attempt++) {
            sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock == INVALID_SOCKET) {
                fprintf(stderr, "[SOCKET] Socket creation failed: %d\n", WSAGetLastError());
                Sleep(500);
                continue;
            }

            sockaddr_in serverAddr;
            serverAddr.sin_family = AF_INET;
            serverAddr.sin_port = htons(port);

            int pton_result = inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr);
            if (pton_result != 1) {
                fprintf(stderr, "[SOCKET] inet_pton failed for host '%s': result=%d, WSAError=%d\n",
                        host.c_str(), pton_result, WSAGetLastError());
                closesocket(sock);
                sock = INVALID_SOCKET;
                Sleep(500);
                continue;
            }

            if (::connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
                int err = WSAGetLastError();
                closesocket(sock);
                sock = INVALID_SOCKET;
                if (attempt < maxRetries) {
                    fprintf(stderr, "[SOCKET] Connection failed (attempt %d/%d), retrying... (WSAError=%d)\n", 
                            attempt, maxRetries, err);
                    Sleep(500);
                } else {
                    fprintf(stderr, "[SOCKET] Failed to connect after %d attempts\n", maxRetries);
                    return false;
                }
            } else {
                printf("[SOCKET] ✓ Connected to %s:%d (attempt %d/%d)\n", 
                       host.c_str(), port, attempt, maxRetries);
                connected = true;
                return true;
            }
        }
        return false;
    }

    void sendMessage(const std::string& type, int blockId, const std::string& blockName, 
                    const std::string& data, int pid = 0) {
        if (!connected) return;

        std::ostringstream json;
        json << "{"
             << "\"protocol\":\"CPP_V1\","
             << "\"timestamp\":\"" << getCurrentTimestamp() << "\","
             << "\"blockId\":" << blockId << ","
             << "\"blockName\":\"" << blockName << "\","
             << "\"type\":\"" << type << "\"";
        
        if (pid > 0) {
            json << ",\"pid\":" << pid;
        }
        
        json << ",\"data\":";
        
        if (type == "BLOCK_METRICS") {
            json << data;
        } else {
            json << "{\"status\":\"" << data << "\"}";
        }
        
        json << "}\n";

        std::string message = json.str();
        send(sock, message.c_str(), (int)message.length(), 0);
    }

    void sendInit(int blockId, const std::string& blockName, int pid) {
        sendMessage("BLOCK_INIT", blockId, blockName, "initializing", pid);
    }

    void sendReady(int blockId, const std::string& blockName) {
        sendMessage("BLOCK_READY", blockId, blockName, "ready");
    }

    void sendError(int blockId, const std::string& blockName, const std::string& error) {
        sendMessage("BLOCK_ERROR", blockId, blockName, error);
    }

    void sendMetrics(int blockId, const std::string& blockName, 
                    int frames, double gbps, double totalGB = -1) {
        std::ostringstream data;
        data << "{\"frames\":" << frames 
             << ",\"gbps\":" << gbps
             << ",\"totalGB\":" << totalGB << "}";
        sendMessage("BLOCK_METRICS", blockId, blockName, data.str());
    }

    void sendStopping(int blockId, const std::string& blockName) {
        sendMessage("BLOCK_STOPPING", blockId, blockName, "stopping");
    }

    void sendStopped(int blockId, const std::string& blockName) {
        sendMessage("BLOCK_STOPPED", blockId, blockName, "stopped");
    }
};

// Get environment variable with default value
inline int getEnvInt(const char* name, int defaultValue) {
    char* value = getenv(name);
    return value ? atoi(value) : defaultValue;
}

inline std::string getEnvString(const char* name, const std::string& defaultValue) {
    char* value = getenv(name);
    return value ? std::string(value) : defaultValue;
}

#endif // CPP_SOCKET_CLIENT_H
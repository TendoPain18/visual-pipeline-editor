#ifndef RUN_GENERIC_BLOCK_H
#define RUN_GENERIC_BLOCK_H

#include "cpp_socket_client.h"
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string>
#include <vector>

struct BlockConfig {
    const char* name;
    int inputs;
    int outputs;
    std::vector<int> inputPacketSizes;
    std::vector<int> inputBatchSizes;
    std::vector<int> outputPacketSizes;
    std::vector<int> outputBatchSizes;
    bool ltr;
    bool startWithAll;
    const char* description;
};

// Calculate length bytes needed for batch count
inline int calculateLengthBytes(int maxCount) {
    if (maxCount <= 255) return 1;
    if (maxCount <= 65535) return 2;
    if (maxCount <= 16777215) return 3;
    return 4;
}

// Calculate total buffer size
inline int calculateBufferSize(int packetSize, int batchSize) {
    int lengthBytes = calculateLengthBytes(batchSize);
    return lengthBytes + (packetSize * batchSize);
}

// Low-level pipe read
inline int readBatch(const char* pipeName, int8_t* batchData, int lengthBytes, int totalSize) {
    std::string instanceId = getEnvString("INSTANCE_ID", "");
    std::string fullPipeName = "Instance_" + instanceId + "_" + std::string(pipeName);
    
    char readyName[256], emptyName[256];
    sprintf(readyName, "%s_Ready", fullPipeName.c_str());
    sprintf(emptyName, "%s_Empty", fullPipeName.c_str());
    
    HANDLE hMapFile = OpenFileMappingA(FILE_MAP_READ, FALSE, fullPipeName.c_str());
    if (!hMapFile) {
        fprintf(stderr, "[PIPE] Failed to open %s: %lu\n", fullPipeName.c_str(), GetLastError());
        return 0;
    }
    
    void* pBuf = MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, totalSize);
    if (!pBuf) {
        CloseHandle(hMapFile);
        return 0;
    }
    
    HANDLE hReady = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, readyName);
    HANDLE hEmpty = OpenEventA(EVENT_MODIFY_STATE, FALSE, emptyName);
    
    WaitForSingleObject(hReady, INFINITE);
    
    uint8_t* buffer = (uint8_t*)pBuf;
    int actualCount = 0;
    for (int i = 0; i < lengthBytes; i++) {
        actualCount |= buffer[i] << (i * 8);
    }
    
    memcpy(batchData, buffer + lengthBytes, totalSize - lengthBytes);
    
    SetEvent(hEmpty);
    
    UnmapViewOfFile(pBuf);
    CloseHandle(hMapFile);
    CloseHandle(hReady);
    CloseHandle(hEmpty);
    
    return actualCount;
}

// Low-level pipe write
inline void writeBatch(const char* pipeName, int8_t* batchData, int actualCount, int lengthBytes, int totalSize) {
    std::string instanceId = getEnvString("INSTANCE_ID", "");
    std::string fullPipeName = "Instance_" + instanceId + "_" + std::string(pipeName);
    
    char readyName[256], emptyName[256];
    sprintf(readyName, "%s_Ready", fullPipeName.c_str());
    sprintf(emptyName, "%s_Empty", fullPipeName.c_str());
    
    HANDLE hMapFile = OpenFileMappingA(FILE_MAP_WRITE, FALSE, fullPipeName.c_str());
    if (!hMapFile) {
        fprintf(stderr, "[PIPE] Failed to open %s: %lu\n", fullPipeName.c_str(), GetLastError());
        return;
    }
    
    void* pBuf = MapViewOfFile(hMapFile, FILE_MAP_WRITE, 0, 0, totalSize);
    if (!pBuf) {
        CloseHandle(hMapFile);
        return;
    }
    
    HANDLE hEmpty = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, emptyName);
    HANDLE hReady = OpenEventA(EVENT_MODIFY_STATE, FALSE, readyName);
    
    WaitForSingleObject(hEmpty, INFINITE);
    
    uint8_t* buffer = (uint8_t*)pBuf;
    for (int i = 0; i < lengthBytes; i++) {
        buffer[i] = (actualCount >> (i * 8)) & 0xFF;
    }
    
    memcpy(buffer + lengthBytes, batchData, totalSize - lengthBytes);
    
    SetEvent(hReady);
    
    UnmapViewOfFile(pBuf);
    CloseHandle(hMapFile);
    CloseHandle(hEmpty);
    CloseHandle(hReady);
}

// ============================================================================
// PIPE I/O HELPER CLASS - Easy manual pipe operations
// ============================================================================

class PipeIO {
private:
    const char* pipeName;
    int lengthBytes;
    int packetSize;
    int batchSize;
    int bufferSize;
    
public:
    PipeIO(const char* pipe, int pktSize, int batchSz) 
        : pipeName(pipe), packetSize(pktSize), batchSize(batchSz) {
        lengthBytes = calculateLengthBytes(batchSz);
        bufferSize = lengthBytes + (pktSize * batchSz);
    }
    
    // Read a batch - returns number of packets read
    int read(int8_t* buffer) {
        return readBatch(pipeName, buffer, lengthBytes, bufferSize);
    }
    
    // Write a batch
    void write(int8_t* buffer, int actualCount) {
        writeBatch(pipeName, buffer, actualCount, lengthBytes, bufferSize);
    }
    
    int getBufferSize() const { return bufferSize; }
    int getPacketSize() const { return packetSize; }
    int getBatchSize() const { return batchSize; }
    int getLengthBytes() const { return lengthBytes; }
    const char* getName() const { return pipeName; }
};

// ============================================================================
// MANUAL I/O BLOCK RUNNER - Full control over read/write timing
// ============================================================================

template<typename CustomData>
void run_manual_block(
    const char** pipeIn,
    const char** pipeOut,
    const BlockConfig& config,
    void (*process_fn)(const char**, const char**, CustomData&, const BlockConfig&),
    CustomData (*init_fn)(const BlockConfig&) = nullptr
) {
    // Get block ID from environment
    int blockId = getEnvInt("BLOCK_ID", 0);
    int cppPort = getEnvInt("CPP_PORT", 9002);
    
    // Connect to socket
    CppSocketClient socket("127.0.0.1", cppPort);
    if (!socket.connect(10)) {
        fprintf(stderr, "[BLOCK] Failed to connect to socket server\n");
        return;
    }
    
    // Get process PID
    DWORD pid = GetCurrentProcessId();
    
    // Send init message with PID
    socket.sendInit(blockId, config.name, pid);
    
    printf("\n========================================\n");
    printf("%s - MANUAL I/O MODE\n", config.name);
    printf("========================================\n");
    printf("Block has full control over read/write timing\n");
    
    bool isSource = (config.inputs == 0);
    bool isSink = (config.outputs == 0);
    
    if (!isSource) {
        printf("INPUTS: %d\n", config.inputs);
        for (int i = 0; i < config.inputs; i++) {
            int bufSize = calculateBufferSize(config.inputPacketSizes[i], config.inputBatchSizes[i]);
            printf("  Input %d: %d bytes × %d packets = %.2f KB\n",
                   i, config.inputPacketSizes[i], config.inputBatchSizes[i], bufSize / 1024.0);
        }
    }
    
    if (!isSink) {
        printf("OUTPUTS: %d\n", config.outputs);
        for (int i = 0; i < config.outputs; i++) {
            int bufSize = calculateBufferSize(config.outputPacketSizes[i], config.outputBatchSizes[i]);
            printf("  Output %d: %d bytes × %d packets = %.2f KB\n",
                   i, config.outputPacketSizes[i], config.outputBatchSizes[i], bufSize / 1024.0);
        }
    }
    
    printf("========================================\n\n");
    
    // Custom initialization
    CustomData customData;
    if (init_fn) {
        printf("Running custom initialization...\n");
        customData = init_fn(config);
    }
    
    socket.sendReady(blockId, config.name);
    
    // Metrics
    int iterationCount = 0;
    double totalBytes = 0;
    LARGE_INTEGER freq, startTime, lastTime;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&startTime);
    lastTime = startTime;
    
    // Main loop - block controls EVERYTHING
    try {
        while (true) {
            // Block's processing function controls all I/O
            process_fn(pipeIn, pipeOut, customData, config);
            
            iterationCount++;
            
            // Estimate throughput (rough)
            if (!isSink && config.outputs > 0) {
                totalBytes += calculateBufferSize(config.outputPacketSizes[0], config.outputBatchSizes[0]);
            } else if (!isSource && config.inputs > 0) {
                totalBytes += calculateBufferSize(config.inputPacketSizes[0], config.inputBatchSizes[0]);
            }
            
            // Send metrics
            LARGE_INTEGER currentTime;
            QueryPerformanceCounter(&currentTime);
            double elapsed = (double)(currentTime.QuadPart - lastTime.QuadPart) / freq.QuadPart;
            
            if (elapsed > 0) {
                double instantGbps = ((totalBytes * 8.0) / 1e9) / elapsed;
                socket.sendMetrics(blockId, config.name, iterationCount, instantGbps, totalBytes / 1e9);
                lastTime = currentTime;
            }
            
            if (iterationCount % 100 == 0) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                double totalElapsed = (double)(now.QuadPart - startTime.QuadPart) / freq.QuadPart;
                double avgGbps = ((totalBytes * 8.0) / 1e9) / totalElapsed;
                printf("Iterations: %d, Avg Throughput: %.2f Gbps\n", iterationCount, avgGbps);
            }
        }
    } catch (...) {
        printf("\nBlock finished\n");
        printf("Total iterations: %d\n", iterationCount);
        printf("Total data: %.2f GB\n", totalBytes / 1e9);
        
        socket.sendStopped(blockId, config.name);
    }
}

#endif // RUN_GENERIC_BLOCK_H
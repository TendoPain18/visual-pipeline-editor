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

// Write batch to pipe with length header
inline void writeBatch(const char* pipeName, int8_t* batchData, int actualCount, 
                      int lengthBytes, int totalSize) {
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

// Read batch from pipe with length header
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

// Generic block runner
template<typename CustomData>
void run_generic_block(
    const char** pipeIn,
    const char** pipeOut,
    const BlockConfig& config,
    void (*process_fn)(int8_t*, int, int8_t*, int&, CustomData&, const BlockConfig&),
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
    printf("%s - Generic Block Framework\n", config.name);
    printf("========================================\n");
    
    bool isSource = (config.inputs == 0);
    bool isSink = (config.outputs == 0);
    
    // Calculate buffer parameters
    int inputPacketSize = 0, inputBatchSize = 0, inputLengthBytes = 0, inputBufferSize = 0;
    int outputPacketSize = 0, outputBatchSize = 0, outputLengthBytes = 0, outputBufferSize = 0;
    
    if (!isSource) {
        inputPacketSize = config.inputPacketSizes[0];
        inputBatchSize = config.inputBatchSizes[0];
        inputLengthBytes = calculateLengthBytes(inputBatchSize);
        inputBufferSize = inputLengthBytes + (inputPacketSize * inputBatchSize);
        printf("INPUT:  %d bytes × %d packets (header: %dB, total: %.2fKB)\n",
               inputPacketSize, inputBatchSize, inputLengthBytes, inputBufferSize/1024.0);
    }
    
    if (!isSink) {
        outputPacketSize = config.outputPacketSizes[0];
        outputBatchSize = config.outputBatchSizes[0];
        outputLengthBytes = calculateLengthBytes(outputBatchSize);
        outputBufferSize = outputLengthBytes + (outputPacketSize * outputBatchSize);
        printf("OUTPUT: %d bytes × %d packets (header: %dB, total: %.2fKB)\n",
               outputPacketSize, outputBatchSize, outputLengthBytes, outputBufferSize/1024.0);
    }
    
    printf("========================================\n\n");
    
    // Custom initialization
    CustomData customData;
    if (init_fn) {
        printf("Running custom initialization...\n");
        customData = init_fn(config);
    }
    
    socket.sendReady(blockId, config.name);
    
    // Allocate buffers
    int8_t* inputBatch = isSource ? nullptr : new int8_t[inputBufferSize];
    int8_t* outputBatch = isSink ? nullptr : new int8_t[outputBufferSize];
    
    // Metrics
    int batchCount = 0;
    double totalBytes = 0;
    LARGE_INTEGER freq, startTime, lastTime;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&startTime);
    lastTime = startTime;
    
    // Main loop
    try {
        while (true) {
            int actualInputCount = 0;
            
            // Read input
            if (!isSource) {
                actualInputCount = readBatch(pipeIn[0], inputBatch, inputLengthBytes, inputBufferSize);
            }
            
            // Process
            int actualOutputCount = 0;
            process_fn(inputBatch, actualInputCount, outputBatch, actualOutputCount, customData, config);
            
            // Write output
            if (!isSink && outputBatch) {
                writeBatch(pipeOut[0], outputBatch, actualOutputCount, outputLengthBytes, outputBufferSize);
            }
            
            batchCount++;
            
            // Calculate throughput
            if (!isSink) {
                totalBytes += outputBufferSize;
            } else if (!isSource) {
                totalBytes += inputBufferSize;
            }
            
            LARGE_INTEGER currentTime;
            QueryPerformanceCounter(&currentTime);
            double elapsed = (double)(currentTime.QuadPart - lastTime.QuadPart) / freq.QuadPart;
            
            if (elapsed > 0) {
                double instantGbps = ((totalBytes * 8.0) / 1e9) / elapsed;
                socket.sendMetrics(blockId, config.name, batchCount, instantGbps, totalBytes / 1e9);
                lastTime = currentTime;
            }
            
            if (batchCount % 100 == 0) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                double totalElapsed = (double)(now.QuadPart - startTime.QuadPart) / freq.QuadPart;
                double avgGbps = ((totalBytes * 8.0) / 1e9) / totalElapsed;
                printf("Batches: %d, Avg Throughput: %.2f Gbps\n", batchCount, avgGbps);
            }
        }
    } catch (...) {
        printf("\nBlock finished\n");
        printf("Total batches: %d\n", batchCount);
        printf("Total data: %.2f GB\n", totalBytes / 1e9);
        
        socket.sendStopped(blockId, config.name);
    }
    
    // Cleanup
    if (inputBatch) delete[] inputBatch;
    if (outputBatch) delete[] outputBatch;
}

#endif // RUN_GENERIC_BLOCK_H
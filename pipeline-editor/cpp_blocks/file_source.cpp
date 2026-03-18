#include "core/run_generic_block.h"
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

const uint8_t START_FLAG[] = {0xAA, 0x55, 0xAA, 0x55};
const uint8_t END_FLAG[] = {0x55, 0xAA, 0x55, 0xAA};
const int FILENAME_LENGTH = 256;
const int REPETITIONS = 10;

struct FileSourceData {
    std::vector<std::string> filePaths;
    int currentFileIdx;
    std::vector<uint8_t> streamBuffer;
    std::string sourceDirectory;
};

FileSourceData init_file_source(const BlockConfig& config) {
    FileSourceData data;
    data.sourceDirectory = "Test_Files";
    data.currentFileIdx = 0;
    
    if (!fs::exists(data.sourceDirectory)) {
        fs::create_directories(data.sourceDirectory);
        fprintf(stderr, "Created source directory. Add files and restart.\n");
        exit(1);
    }
    
    for (const auto& entry : fs::directory_iterator(data.sourceDirectory)) {
        if (entry.is_regular_file()) {
            data.filePaths.push_back(entry.path().string());
        }
    }
    
    if (data.filePaths.empty()) {
        fprintf(stderr, "No files found in source directory.\n");
        exit(1);
    }
    
    printf("Found %zu file(s) to send\n", data.filePaths.size());
    
    return data;
}

void process_file_source(
    const char** pipeIn,
    const char** pipeOut,
    FileSourceData& customData,
    const BlockConfig& config
) {
    // Create output pipe handler
    PipeIO output(pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);
    
    int packetSize = output.getPacketSize();
    int batchSize = output.getBatchSize();
    int totalBatchBytes = batchSize * packetSize;
    
    // Allocate output buffer
    int8_t* outputBatch = new int8_t[output.getBufferSize()];
    
    // Fill buffer with files
    while (customData.currentFileIdx < customData.filePaths.size() && 
           customData.streamBuffer.size() < totalBatchBytes) {
        
        std::string filePath = customData.filePaths[customData.currentFileIdx];
        std::string fileName = fs::path(filePath).filename().string();
        
        std::ifstream file(filePath, std::ios::binary);
        if (!file) {
            fprintf(stderr, "Cannot open: %s (skipping)\n", fileName.c_str());
            customData.currentFileIdx++;
            continue;
        }
        
        std::vector<uint8_t> fileData((std::istreambuf_iterator<char>(file)),
                                      std::istreambuf_iterator<char>());
        file.close();
        
        int fileSize = fileData.size();
        printf("Loading file %d/%zu: %s (%.2f KB)\n",
               customData.currentFileIdx + 1, customData.filePaths.size(),
               fileName.c_str(), fileSize / 1024.0);
        
        // Build file packet
        std::vector<uint8_t> fileStream;
        
        fileStream.insert(fileStream.end(), START_FLAG, START_FLAG + 4);
        
        for (int rep = 0; rep < REPETITIONS; rep++) {
            std::vector<uint8_t> nameBytes(FILENAME_LENGTH, 0);
            int nameLen = std::min((int)fileName.length(), FILENAME_LENGTH);
            memcpy(nameBytes.data(), fileName.c_str(), nameLen);
            fileStream.insert(fileStream.end(), nameBytes.begin(), nameBytes.end());
        }
        
        for (int rep = 0; rep < REPETITIONS; rep++) {
            uint64_t size64 = fileSize;
            uint8_t* sizeBytes = (uint8_t*)&size64;
            fileStream.insert(fileStream.end(), sizeBytes, sizeBytes + 8);
        }
        
        fileStream.insert(fileStream.end(), fileData.begin(), fileData.end());
        fileStream.insert(fileStream.end(), END_FLAG, END_FLAG + 4);
        
        customData.streamBuffer.insert(customData.streamBuffer.end(), 
                                       fileStream.begin(), fileStream.end());
        
        printf("  Added to buffer: %zu bytes (total buffer: %.2f KB)\n",
               fileStream.size(), customData.streamBuffer.size() / 1024.0);
        
        customData.currentFileIdx++;
        
        if (customData.streamBuffer.size() >= totalBatchBytes) {
            break;
        }
    }
    
    // Check if done
    if (customData.currentFileIdx >= customData.filePaths.size() && 
        customData.streamBuffer.empty()) {
        printf("\n========================================\n");
        printf("TRANSMISSION COMPLETE\n");
        printf("========================================\n");
        printf("Total files sent: %zu\n", customData.filePaths.size());
        printf("========================================\n");
        throw std::runtime_error("All files transmitted");
    }
    
    // Prepare batch
    int availableBytes = customData.streamBuffer.size();
    int maxPacketsFromBuffer = (availableBytes + packetSize - 1) / packetSize;
    int packetsInThisBatch = std::min(batchSize, maxPacketsFromBuffer);
    
    if (packetsInThisBatch == 0) {
        delete[] outputBatch;
        return;
    }
    
    memset(outputBatch, 0, output.getBufferSize());
    int bytesToSend = std::min(packetsInThisBatch * packetSize, availableBytes);
    
    for (int i = 0; i < bytesToSend; i++) {
        outputBatch[i] = (int8_t)((int32_t)customData.streamBuffer[i] - 128);
    }
    
    customData.streamBuffer.erase(customData.streamBuffer.begin(), 
                                   customData.streamBuffer.begin() + bytesToSend);
    
    printf("Sending batch (%d packets, buffer remaining: %.2f KB)\n",
           packetsInThisBatch, customData.streamBuffer.size() / 1024.0);
    
    // MANUAL WRITE - Block controls when to send
    output.write(outputBatch, packetsInThisBatch);
    
    delete[] outputBatch;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: file_source <pipeOut>\n");
        return 1;
    }
    
    const char* pipeOut = argv[1];
    
    BlockConfig config = {
        "FileSource",       // name
        0,                  // inputs
        1,                  // outputs
        {},                 // inputPacketSizes
        {},                 // inputBatchSizes
        {1500},             // outputPacketSizes
        {6000},            // outputBatchSizes
        true,               // ltr
        false,              // startWithAll (manual start for sources)
        "Progressive file loading with continuous buffering"  // description
    };
    
    run_manual_block(nullptr, &pipeOut, config, process_file_source, init_file_source);
    
    return 0;
}
#include "core/run_generic_block.h"
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <map>

namespace fs = std::filesystem;

// File protocol constants
const uint8_t START_FLAG[] = {0xAA, 0x55, 0xAA, 0x55};
const uint8_t END_FLAG[] = {0x55, 0xAA, 0x55, 0xAA};
const int FILENAME_LENGTH = 256;
const int REPETITIONS = 10;

struct FileInfo {
    std::string name;
    std::string tempPath;
    std::string finalPath;
    uint64_t expectedSize;
    uint64_t writtenBytes;
    int errorCount;
    std::ofstream file;
    bool active;
};

struct FileSinkData {
    std::vector<uint8_t> streamBuffer;
    FileInfo currentFile;
    int filesReceived;
    int totalErrorCount;
    std::string outputDirectory;
    std::ofstream reportFile;
};

// Helper: Find pattern in buffer
int find_pattern(const std::vector<uint8_t>& data, const uint8_t* pattern, int patternLen) {
    for (size_t i = 0; i <= data.size() - patternLen; i++) {
        if (memcmp(&data[i], pattern, patternLen) == 0) {
            return i;
        }
    }
    return -1;
}

// Helper: Majority vote for string
std::string majority_vote_string(const std::vector<std::string>& strings) {
    if (strings.empty()) return "";
    
    std::map<std::string, int> counts;
    for (const auto& s : strings) {
        counts[s]++;
    }
    
    int maxCount = 0;
    std::string result;
    for (const auto& pair : counts) {
        if (pair.second > maxCount) {
            maxCount = pair.second;
            result = pair.first;
        }
    }
    return result;
}

FileSinkData init_file_sink(const BlockConfig& config) {
    FileSinkData data;
    data.outputDirectory = "Output_Files";
    data.filesReceived = 0;
    data.totalErrorCount = 0;
    data.currentFile.active = false;
    
    // Create output directory
    if (!fs::exists(data.outputDirectory)) {
        fs::create_directories(data.outputDirectory);
    }
    
    // Create error report
    std::string reportPath = data.outputDirectory + "/error_report.txt";
    data.reportFile.open(reportPath, std::ios::out);
    data.reportFile << "========================================\n";
    data.reportFile << "FILE TRANSMISSION ERROR REPORT\n";
    data.reportFile << "========================================\n";
    data.reportFile << "Started: " << __DATE__ << " " << __TIME__ << "\n";
    data.reportFile << "Mode: STREAMING (on-the-fly writes)\n\n";
    data.reportFile << "FILES RECEIVED:\n";
    data.reportFile << "========================================\n\n";
    data.reportFile.flush();
    
    printf("Output directory: %s\n", data.outputDirectory.c_str());
    printf("Error report:     %s\n", reportPath.c_str());
    printf("Streaming mode active - writing to disk on-the-fly...\n\n");
    
    return data;
}

void process_file_sink(int8_t* inputBatch, int actualCount, int8_t* outputBatch,
                      int& actualOutputCount, FileSinkData& customData, const BlockConfig& config) {
    
    int packetSize = config.inputPacketSizes[0];
    int dataSize = packetSize - 1;  // Last byte is error flag
    
    // Handle EOF signal
    if (actualCount == 0) {
        printf("Received EOF signal (0 packets)\n");
        
        if (customData.currentFile.active && customData.currentFile.file.is_open()) {
            customData.currentFile.file.close();
            if (fs::exists(customData.currentFile.tempPath)) {
                fs::rename(customData.currentFile.tempPath, customData.currentFile.finalPath);
            }
            printf("Completed: %s\n", customData.currentFile.name.c_str());
            customData.filesReceived++;
            customData.currentFile.active = false;
        }
        
        printf("\n========================================\n");
        printf("ALL FILES RECEIVED\n");
        printf("========================================\n");
        printf("Total files: %d\n", customData.filesReceived);
        printf("========================================\n");
        
        actualOutputCount = 0;
        return;
    }
    
    // Extract data from batch
    std::vector<uint8_t> newData;
    for (int i = 0; i < actualCount; i++) {
        int offset = i * packetSize;
        for (int j = 0; j < dataSize; j++) {
            newData.push_back((uint8_t)((int32_t)inputBatch[offset + j] + 128));
        }
        
        // Track errors
        if (inputBatch[offset + packetSize - 1] != 0) {
            customData.totalErrorCount++;
        }
    }
    
    // Add to stream buffer
    customData.streamBuffer.insert(customData.streamBuffer.end(), newData.begin(), newData.end());
    
    // Process stream buffer
    while (true) {
        if (!customData.currentFile.active) {
            // Look for START flag
            int startPos = find_pattern(customData.streamBuffer, START_FLAG, 4);
            
            if (startPos < 0) {
                if (customData.streamBuffer.size() > 4096) {
                    customData.streamBuffer.erase(customData.streamBuffer.begin(), 
                                                  customData.streamBuffer.end() - 4095);
                }
                break;
            }
            
            // Parse metadata
            int metadataSize = 4 + (FILENAME_LENGTH * REPETITIONS) + (8 * REPETITIONS);
            
            if (customData.streamBuffer.size() < startPos + metadataSize) {
                break;
            }
            
            // Extract filename (majority vote)
            std::vector<std::string> fileNames;
            int nameStart = startPos + 4;
            for (int i = 0; i < REPETITIONS; i++) {
                std::vector<uint8_t> nameBytes(FILENAME_LENGTH);
                memcpy(nameBytes.data(), &customData.streamBuffer[nameStart + i * FILENAME_LENGTH], FILENAME_LENGTH);
                
                // Find null terminator
                auto nullPos = std::find(nameBytes.begin(), nameBytes.end(), 0);
                if (nullPos != nameBytes.end()) {
                    nameBytes.erase(nullPos, nameBytes.end());
                }
                
                fileNames.push_back(std::string(nameBytes.begin(), nameBytes.end()));
            }
            std::string fileName = majority_vote_string(fileNames);
            
            // Extract file size (mode)
            std::vector<uint64_t> fileSizes;
            int sizeStart = nameStart + (FILENAME_LENGTH * REPETITIONS);
            for (int i = 0; i < REPETITIONS; i++) {
                uint64_t size;
                memcpy(&size, &customData.streamBuffer[sizeStart + i * 8], 8);
                fileSizes.push_back(size);
            }
            
            // Find mode (most common value)
            std::map<uint64_t, int> sizeCounts;
            for (uint64_t size : fileSizes) sizeCounts[size]++;
            uint64_t fileSize = 0;
            int maxCount = 0;
            for (const auto& pair : sizeCounts) {
                if (pair.second > maxCount) {
                    maxCount = pair.second;
                    fileSize = pair.first;
                }
            }
            
            // Create file paths
            std::string safeFileName = fileName;
            // Remove invalid characters
            std::replace_if(safeFileName.begin(), safeFileName.end(), 
                          [](char c) { return c == '\\' || c == '/' || c == ':' || 
                                             c == '*' || c == '?' || c == '"' || 
                                             c == '<' || c == '>' || c == '|'; }, '_');
            
            std::string tempPath = customData.outputDirectory + "/" + safeFileName + ".part";
            std::string finalPath = customData.outputDirectory + "/" + safeFileName;
            
            // Open file
            customData.currentFile.file.open(tempPath, std::ios::binary);
            if (!customData.currentFile.file) {
                fprintf(stderr, "Cannot create file: %s\n", tempPath.c_str());
                break;
            }
            
            printf("Started streaming: %s (%.2f MB)\n", fileName.c_str(), fileSize / 1e6);
            
            customData.currentFile.active = true;
            customData.currentFile.name = fileName;
            customData.currentFile.tempPath = tempPath;
            customData.currentFile.finalPath = finalPath;
            customData.currentFile.expectedSize = fileSize;
            customData.currentFile.writtenBytes = 0;
            customData.currentFile.errorCount = 0;
            
            // Remove metadata from buffer
            int dataStart = startPos + metadataSize;
            customData.streamBuffer.erase(customData.streamBuffer.begin(), 
                                         customData.streamBuffer.begin() + dataStart);
            
        } else {
            // Write data to file
            uint64_t remainingBytes = customData.currentFile.expectedSize - 
                                     customData.currentFile.writtenBytes;
            
            if (remainingBytes <= 0) {
                // File complete - look for END flag
                if (customData.streamBuffer.size() >= 4) {
                    bool endFlagMatch = memcmp(customData.streamBuffer.data(), END_FLAG, 4) == 0;
                    
                    if (endFlagMatch) {
                        customData.streamBuffer.erase(customData.streamBuffer.begin(), 
                                                     customData.streamBuffer.begin() + 4);
                        
                        customData.currentFile.file.close();
                        fs::rename(customData.currentFile.tempPath, customData.currentFile.finalPath);
                        
                        printf("Completed: %s (%.2f MB)", customData.currentFile.name.c_str(), 
                               customData.currentFile.expectedSize / 1e6);
                        
                        if (customData.currentFile.errorCount > 0) {
                            double errorRate = 100.0 * customData.currentFile.errorCount / 
                                             customData.currentFile.expectedSize;
                            printf(" - ⚠ %d errors (%.4f%%)\n", 
                                   customData.currentFile.errorCount, errorRate);
                        } else {
                            printf(" - ✓ Clean\n");
                        }
                        
                        customData.filesReceived++;
                        customData.currentFile.active = false;
                    } else {
                        fprintf(stderr, "WARNING: END flag mismatch for %s\n", 
                               customData.currentFile.name.c_str());
                        customData.currentFile.file.close();
                        customData.currentFile.active = false;
                        customData.streamBuffer.erase(customData.streamBuffer.begin(), 
                                                     customData.streamBuffer.begin() + 4);
                    }
                } else {
                    break;
                }
            } else {
                // Write available data
                int bytesToWrite = std::min((uint64_t)customData.streamBuffer.size(), remainingBytes);
                
                if (bytesToWrite > 0) {
                    customData.currentFile.file.write((char*)customData.streamBuffer.data(), bytesToWrite);
                    customData.currentFile.writtenBytes += bytesToWrite;
                    customData.streamBuffer.erase(customData.streamBuffer.begin(), 
                                                 customData.streamBuffer.begin() + bytesToWrite);
                    
                    // Show progress at 10% intervals
                    int progress = (int)(100.0 * customData.currentFile.writtenBytes / 
                                        customData.currentFile.expectedSize);
                    static int lastProgress = -1;
                    if (progress % 10 == 0 && progress != lastProgress) {
                        printf("  Progress: %s - %d%% (%.2f MB / %.2f MB)\n",
                               customData.currentFile.name.c_str(), progress,
                               customData.currentFile.writtenBytes / 1e6,
                               customData.currentFile.expectedSize / 1e6);
                        lastProgress = progress;
                    }
                } else {
                    break;
                }
            }
        }
    }
    
    actualOutputCount = 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: file_sink <pipeIn>\n");
        return 1;
    }
    
    const char* pipeIn = argv[1];
    
    BlockConfig config = {
        "FileSink",         // name
        1,                  // inputs
        0,                  // outputs
        {1501},             // inputPacketSizes
        {44740},            // inputBatchSizes
        {},                 // outputPacketSizes
        {},                 // outputBatchSizes
        false,              // ltr
        true,               // startWithAll (AUTO-START)
        "Streaming file sink with on-the-fly disk writes"  // description
    };
    
    run_generic_block(&pipeIn, nullptr, config, process_file_sink, init_file_sink);
    
    return 0;
}
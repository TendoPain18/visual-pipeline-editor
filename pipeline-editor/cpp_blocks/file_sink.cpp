#include "core/run_generic_block.h"
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <map>
#include <ctime>

namespace fs = std::filesystem;

const uint8_t START_FLAG[] = {0xAA, 0x55, 0xAA, 0x55};
const uint8_t END_FLAG[]   = {0x55, 0xAA, 0x55, 0xAA};
const int FILENAME_LENGTH  = 256;
const int REPETITIONS      = 10;

struct FileInfo {
    std::string name;
    std::string tempPath;
    std::string finalPath;
    uint64_t    expectedSize;
    uint64_t    writtenBytes;
    uint64_t    errorCount;     // <-- tracks CRC/error-flag bytes per file
    std::ofstream file;
    bool        active;
};

struct FileSinkData {
    std::vector<uint8_t> streamBuffer;
    FileInfo             currentFile;
    int                  filesReceived;
    uint64_t             totalErrorCount;
    std::string          outputDirectory;
    std::string          reportPath;
};

static int find_pattern(const std::vector<uint8_t> &data,
                        const uint8_t *pattern, int patternLen) {
    if ((int)data.size() < patternLen) return -1;
    for (int i = 0; i <= (int)data.size() - patternLen; i++) {
        if (memcmp(&data[i], pattern, patternLen) == 0) return i;
    }
    return -1;
}

static std::string majority_vote_string(const std::vector<std::string> &strings) {
    if (strings.empty()) return "";
    std::map<std::string, int> counts;
    for (const auto &s : strings) counts[s]++;
    std::string result;
    int maxCount = 0;
    for (const auto &pair : counts) {
        if (pair.second > maxCount) { maxCount = pair.second; result = pair.first; }
    }
    return result;
}

// ---- Write a completed-file entry to the error report ----
static void append_to_report(const std::string &reportPath, const FileInfo &fi) {
    std::ofstream rpt(reportPath, std::ios::app);
    if (!rpt.is_open()) return;

    // Timestamp
    time_t now = time(nullptr);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));

    rpt << "[" << tbuf << "]\n";
    rpt << "File: " << fi.name << "\n";
    rpt << "  Size:         " << fi.expectedSize << " bytes ("
        << (fi.expectedSize / 1e6) << " MB)\n";
    rpt << "  Errors:       " << fi.errorCount << "\n";
    if (fi.errorCount > 0) {
        double rate = 100.0 * (double)fi.errorCount / (double)fi.expectedSize;
        rpt << "  Error Rate:   " << rate << " %\n";
        rpt << "  Status:       CORRUPTED\n";
    } else {
        rpt << "  Status:       CLEAN\n";
    }
    rpt << "\n";
    rpt.flush();
}

FileSinkData init_file_sink(const BlockConfig &config) {
    FileSinkData data;
    data.outputDirectory = "Output_Files";
    data.filesReceived   = 0;
    data.totalErrorCount = 0;
    data.currentFile.active      = false;
    data.currentFile.errorCount  = 0;
    data.currentFile.writtenBytes= 0;

    if (!fs::exists(data.outputDirectory)) {
        fs::create_directories(data.outputDirectory);
    }

    data.reportPath = data.outputDirectory + "/error_report.txt";

    // Write report header (overwrite any old file)
    {
        std::ofstream rpt(data.reportPath, std::ios::out);
        time_t now = time(nullptr);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
        rpt << "========================================\n";
        rpt << "FILE TRANSMISSION ERROR REPORT\n";
        rpt << "========================================\n";
        rpt << "Started: " << tbuf << "\n";
        rpt << "Mode: STREAMING (on-the-fly writes)\n\n";
        rpt << "FILES RECEIVED:\n";
        rpt << "========================================\n\n";
    }


    return data;
}

void process_file_sink(
    const char **pipeIn,
    const char ** /*pipeOut*/,
    FileSinkData &customData,
    const BlockConfig &config
) {
    PipeIO input(pipeIn[0], config.inputPacketSizes[0], config.inputBatchSizes[0]);

    int8_t *inputBatch = new int8_t[input.getBufferSize()];
    int actualCount = input.read(inputBatch);

    const int packetSize = input.getPacketSize(); // 1501
    const int dataSize   = packetSize - 1;        // 1500 (last byte is error flag)

    // ---- Handle EOF ----
    if (actualCount == 0) {
        printf("[FileSink] Received EOF (0 packets)\n");
        if (customData.currentFile.active && customData.currentFile.file.is_open()) {
            customData.currentFile.file.close();
            if (fs::exists(customData.currentFile.tempPath)) {
                fs::rename(customData.currentFile.tempPath, customData.currentFile.finalPath);
            }
            append_to_report(customData.reportPath, customData.currentFile);
            printf("[FileSink] Completed (EOF): %s\n", customData.currentFile.name.c_str());
            customData.filesReceived++;
            customData.currentFile.active = false;
        }

        // Write summary to report
        {
            std::ofstream rpt(customData.reportPath, std::ios::app);
            time_t now = time(nullptr);
            char tbuf[32];
            strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
            rpt << "========================================\n";
            rpt << "SUMMARY\n";
            rpt << "========================================\n";
            rpt << "Finished:     " << tbuf << "\n";
            rpt << "Total files:  " << customData.filesReceived << "\n";
            rpt << "Total errors: " << customData.totalErrorCount << "\n";
            rpt << "========================================\n";
        }

        printf("\n========================================\n");
        printf("ALL FILES RECEIVED\n");
        printf("========================================\n");
        printf("Total files:  %d\n", customData.filesReceived);
        printf("Total errors: %llu\n", (unsigned long long)customData.totalErrorCount);
        printf("========================================\n");

        delete[] inputBatch;
        throw std::runtime_error("All files received");
    }

    // ---- Extract data + track errors ----
    static bool firstBatch = true;
    if (firstBatch && actualCount > 0) {
        firstBatch = false;
        printf("[FileSink] pkt[0] INPUT : %d bits (data=%d + error_flag=8)\n",
               packetSize * 8, dataSize * 8);
        fflush(stdout);
    }
    std::vector<uint8_t> newData;
    newData.reserve((size_t)actualCount * dataSize);

    for (int i = 0; i < actualCount; i++) {
        int offset = i * packetSize;
        for (int j = 0; j < dataSize; j++) {
            newData.push_back((uint8_t)((int32_t)inputBatch[offset + j] + 128));
        }
        // Last byte is error flag (non-zero = packet had a CRC error)
        if (inputBatch[offset + packetSize - 1] != 0) {
            customData.totalErrorCount++;
            if (customData.currentFile.active) {
                customData.currentFile.errorCount++;
            }
        }
    }

    customData.streamBuffer.insert(customData.streamBuffer.end(), newData.begin(), newData.end());

    // ---- State machine: parse stream buffer ----
    static int lastProgressPercent = -1;

    while (true) {
        if (!customData.currentFile.active) {
            // ---- Look for START flag ----
            int startPos = find_pattern(customData.streamBuffer, START_FLAG, 4);

            if (startPos < 0) {
                // Trim to avoid unbounded growth
                if ((int)customData.streamBuffer.size() > 4096) {
                    customData.streamBuffer.erase(
                        customData.streamBuffer.begin(),
                        customData.streamBuffer.end() - 4095);
                }
                break;
            }

            int metadataSize = 4 + (FILENAME_LENGTH * REPETITIONS) + (8 * REPETITIONS);

            if ((int)customData.streamBuffer.size() < startPos + metadataSize) break;

            // Parse filename (majority vote across REPETITIONS)
            std::vector<std::string> fileNames;
            int nameStart = startPos + 4;
            for (int r = 0; r < REPETITIONS; r++) {
                std::vector<uint8_t> nameBytes(FILENAME_LENGTH);
                memcpy(nameBytes.data(),
                       &customData.streamBuffer[nameStart + r * FILENAME_LENGTH],
                       FILENAME_LENGTH);
                auto nullPos = std::find(nameBytes.begin(), nameBytes.end(), (uint8_t)0);
                if (nullPos != nameBytes.end()) nameBytes.erase(nullPos, nameBytes.end());
                fileNames.push_back(std::string(nameBytes.begin(), nameBytes.end()));
            }
            std::string fileName = majority_vote_string(fileNames);

            // Parse file size (mode across REPETITIONS)
            int sizeStart = nameStart + FILENAME_LENGTH * REPETITIONS;
            std::map<uint64_t, int> sizeCounts;
            for (int r = 0; r < REPETITIONS; r++) {
                uint64_t sz;
                memcpy(&sz, &customData.streamBuffer[sizeStart + r * 8], 8);
                sizeCounts[sz]++;
            }
            uint64_t fileSize = 0;
            int maxCnt = 0;
            for (const auto &kv : sizeCounts) {
                if (kv.second > maxCnt) { maxCnt = kv.second; fileSize = kv.first; }
            }

            // Sanitise filename for filesystem
            std::string safeFileName = fileName;
            std::replace_if(safeFileName.begin(), safeFileName.end(),
                [](char c) { return c=='\\' || c=='/' || c==':' ||
                                     c=='*'  || c=='?' || c=='"' ||
                                     c=='<'  || c=='>' || c=='|'; }, '_');

            std::string tempPath  = customData.outputDirectory + "/" + safeFileName + ".part";
            std::string finalPath = customData.outputDirectory + "/" + safeFileName;

            customData.currentFile.file.open(tempPath, std::ios::binary);
            if (!customData.currentFile.file) {
                fprintf(stderr, "[FileSink] Cannot create: %s\n", tempPath.c_str());
                break;
            }

            printf("[FileSink] Started: %s (%.2f MB)\n", fileName.c_str(), fileSize / 1e6);

            customData.currentFile.active       = true;
            customData.currentFile.name         = fileName;
            customData.currentFile.tempPath     = tempPath;
            customData.currentFile.finalPath    = finalPath;
            customData.currentFile.expectedSize = fileSize;
            customData.currentFile.writtenBytes = 0;
            customData.currentFile.errorCount   = 0;
            lastProgressPercent                 = -1;

            int dataStart = startPos + metadataSize;
            customData.streamBuffer.erase(customData.streamBuffer.begin(),
                                          customData.streamBuffer.begin() + dataStart);

        } else {
            uint64_t remaining = customData.currentFile.expectedSize -
                                 customData.currentFile.writtenBytes;

            if (remaining == 0) {
                // ---- Look for END flag ----
                if ((int)customData.streamBuffer.size() < 4) break;

                if (memcmp(customData.streamBuffer.data(), END_FLAG, 4) == 0) {
                    customData.streamBuffer.erase(customData.streamBuffer.begin(),
                                                  customData.streamBuffer.begin() + 4);

                    customData.currentFile.file.close();
                    fs::rename(customData.currentFile.tempPath, customData.currentFile.finalPath);

                    // ---- Write entry to error report ----
                    append_to_report(customData.reportPath, customData.currentFile);

                    printf("[FileSink] Completed: %s (%.2f MB)",
                           customData.currentFile.name.c_str(),
                           (double)customData.currentFile.expectedSize / 1e6);
                    if (customData.currentFile.errorCount > 0) {
                        double errRate = 100.0 * (double)customData.currentFile.errorCount /
                                                 (double)customData.currentFile.expectedSize;
                        printf(" - %llu errors (%.4f%%)\n",
                               (unsigned long long)customData.currentFile.errorCount, errRate);
                    } else {
                        printf(" - Clean\n");
                    }

                    customData.filesReceived++;
                    customData.currentFile.active = false;
                } else {
                    fprintf(stderr, "[FileSink] WARNING: END flag mismatch for %s\n",
                            customData.currentFile.name.c_str());
                    customData.currentFile.file.close();
                    append_to_report(customData.reportPath, customData.currentFile);
                    customData.currentFile.active = false;
                    customData.streamBuffer.erase(customData.streamBuffer.begin(),
                                                  customData.streamBuffer.begin() + 4);
                }
            } else {
                // ---- Write available data ----
                uint64_t bytesToWrite = std::min((uint64_t)customData.streamBuffer.size(), remaining);
                if (bytesToWrite == 0) break;

                customData.currentFile.file.write(
                    (const char *)customData.streamBuffer.data(), (std::streamsize)bytesToWrite);
                customData.currentFile.writtenBytes += bytesToWrite;
                customData.streamBuffer.erase(customData.streamBuffer.begin(),
                                              customData.streamBuffer.begin() + bytesToWrite);

                int progress = (int)(100.0 * customData.currentFile.writtenBytes /
                                             customData.currentFile.expectedSize);
                if (progress % 10 == 0 && progress != lastProgressPercent) {
                    printf("[FileSink]   %s - %d%% (%.2f / %.2f MB)\n",
                           customData.currentFile.name.c_str(), progress,
                           (double)customData.currentFile.writtenBytes / 1e6,
                           (double)customData.currentFile.expectedSize / 1e6);
                    lastProgressPercent = progress;
                }
            }
        }
    }

    delete[] inputBatch;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: file_sink <pipeIn>\n");
        return 1;
    }
    const char *pipeIn = argv[1];

    BlockConfig config = {
        "FileSink",             // name
        1,                      // inputs
        0,                      // outputs
        {1501},                 // inputPacketSizes
        {64},                // inputBatchSizes
        {},                     // outputPacketSizes
        {},                     // outputBatchSizes
        true,                   // ltr
        true,                   // startWithAll
        "Streaming file sink with error tracking and on-completion error report"
    };

    run_manual_block(&pipeIn, nullptr, config, process_file_sink, init_file_sink);
    return 0;
}
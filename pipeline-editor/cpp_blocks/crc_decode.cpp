#include "core/run_generic_block.h"
#include <vector>

// CRC-32 table builder
std::vector<uint32_t> build_crc32_table() {
    std::vector<uint32_t> table(256);
    uint32_t poly = 0xEDB88320;
    
    for (int i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ poly;
            } else {
                crc >>= 1;
            }
        }
        table[i] = crc;
    }
    return table;
}

// Calculate CRC-32
uint32_t calculate_crc32(const uint8_t* data, int length, const std::vector<uint32_t>& table) {
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < length; i++) {
        uint8_t tableIdx = (crc & 0xFF) ^ data[i];
        crc = (crc >> 8) ^ table[tableIdx];
    }
    return crc ^ 0xFFFFFFFF;
}

// Custom data structure
struct CrcDecodeData {
    std::vector<uint32_t> crcTable;
    int errorCount;
    int packetCount;
};

// Initialization function
CrcDecodeData init_crc_decode(const BlockConfig& config) {
    CrcDecodeData data;
    data.crcTable = build_crc32_table();
    data.errorCount = 0;
    data.packetCount = 0;
    return data;
}

// Processing function
void process_crc_decode(int8_t* inputBatch, int actualCount, int8_t* outputBatch, 
                       int& actualOutputCount, CrcDecodeData& customData, const BlockConfig& config) {
    
    int inputPacketSize = config.inputPacketSizes[0];
    int outputPacketSize = config.outputPacketSizes[0];
    int outputBatchSize = config.outputBatchSizes[0];
    int dataSize = inputPacketSize - 4;  // Remove CRC bytes
    
    // Clear output
    memset(outputBatch, 0, outputBatchSize * outputPacketSize);
    
    // Process each packet
    for (int i = 0; i < actualCount; i++) {
        int inputOffset = i * inputPacketSize;
        int outputOffset = i * outputPacketSize;
        
        // Split data and CRC
        int8_t* dataInt8 = inputBatch + inputOffset;
        int8_t* crcInt8 = inputBatch + inputOffset + dataSize;
        
        // Calculate CRC on data
        uint8_t* dataAsUint8 = (uint8_t*)dataInt8;
        uint32_t calculatedCrc = calculate_crc32(dataAsUint8, dataSize, customData.crcTable);
        
        // Extract received CRC
        uint8_t crcBytes[4];
        for (int j = 0; j < 4; j++) {
            crcBytes[j] = (uint8_t)((int32_t)crcInt8[j] + 128);
        }
        uint32_t receivedCrc = *(uint32_t*)crcBytes;
        
        // Compare CRCs
        int8_t errorFlag = (receivedCrc == calculatedCrc) ? 0 : 1;
        if (errorFlag) {
            customData.errorCount++;
        }
        
        // Copy data to output
        memcpy(outputBatch + outputOffset, dataInt8, dataSize);
        outputBatch[outputOffset + outputPacketSize - 1] = errorFlag;
        
        customData.packetCount++;
    }
    
    actualOutputCount = actualCount;
    
    // Periodic error reporting
    if (customData.packetCount % 100000 == 0) {
        double errorRate = 100.0 * customData.errorCount / customData.packetCount;
        printf("Packets: %d, Errors: %d (%.2f%%)\n", 
               customData.packetCount, customData.errorCount, errorRate);
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: crc_decode <pipeIn> <pipeOut>\n");
        return 1;
    }
    
    const char* pipeIn = argv[1];
    const char* pipeOut = argv[2];
    
    BlockConfig config = {
        "CrcDecode",        // name
        1,                  // inputs
        1,                  // outputs
        {1504},             // inputPacketSizes
        {44740},            // inputBatchSizes
        {1501},             // outputPacketSizes
        {44740},            // outputBatchSizes
        false,              // ltr
        true,               // startWithAll (AUTO-START)
        "CRC-32 decoder with error detection - batch processing"  // description
    };
    
    run_generic_block(&pipeIn, &pipeOut, config, process_crc_decode, init_crc_decode);
    
    return 0;
}
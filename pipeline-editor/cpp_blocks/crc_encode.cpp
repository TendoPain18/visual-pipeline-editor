#include "core/run_generic_block.h"
#include <vector>

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

uint32_t calculate_crc32(const uint8_t* data, int length, const std::vector<uint32_t>& table) {
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < length; i++) {
        uint8_t tableIdx = (crc & 0xFF) ^ data[i];
        crc = (crc >> 8) ^ table[tableIdx];
    }
    return crc ^ 0xFFFFFFFF;
}

struct CrcEncodeData {
    std::vector<uint32_t> crcTable;
};

CrcEncodeData init_crc_encode(const BlockConfig& config) {
    CrcEncodeData data;
    data.crcTable = build_crc32_table();
    return data;
}

void process_crc_encode(
    const char** pipeIn,
    const char** pipeOut,
    CrcEncodeData& customData,
    const BlockConfig& config
) {
    // Create I/O handlers
    PipeIO input(pipeIn[0], config.inputPacketSizes[0], config.inputBatchSizes[0]);
    PipeIO output(pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);
    
    // Allocate buffers
    int8_t* inputBatch = new int8_t[input.getBufferSize()];
    int8_t* outputBatch = new int8_t[output.getBufferSize()];
    
    // MANUAL READ - Block decides when to read
    int actualCount = input.read(inputBatch);
    
    int inputPacketSize = input.getPacketSize();
    int outputPacketSize = output.getPacketSize();
    
    memset(outputBatch, 0, output.getBufferSize());
    
    // Process each packet
    for (int i = 0; i < actualCount; i++) {
        int inputOffset = i * inputPacketSize;
        int outputOffset = i * outputPacketSize;
        
        // Copy input data
        memcpy(outputBatch + outputOffset, inputBatch + inputOffset, inputPacketSize);
        
        // Calculate CRC
        uint8_t* dataAsUint8 = (uint8_t*)(inputBatch + inputOffset);
        uint32_t crc = calculate_crc32(dataAsUint8, inputPacketSize, customData.crcTable);
        
        // Append CRC as 4 bytes
        uint8_t* crcBytes = (uint8_t*)&crc;
        for (int j = 0; j < 4; j++) {
            outputBatch[outputOffset + inputPacketSize + j] = (int8_t)((int32_t)crcBytes[j] - 128);
        }
    }
    
    // MANUAL WRITE - Block decides when to write
    output.write(outputBatch, actualCount);
    
    delete[] inputBatch;
    delete[] outputBatch;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: crc_encode <pipeIn> <pipeOut>\n");
        return 1;
    }
    
    const char* pipeIn = argv[1];
    const char* pipeOut = argv[2];
    
    BlockConfig config = {
        "CrcEncode",        // name
        1,                  // inputs
        1,                  // outputs
        {1500},             // inputPacketSizes
        {64},            // inputBatchSizes
        {1504},             // outputPacketSizes
        {64},            // outputBatchSizes
        true,               // ltr
        true,               // startWithAll (AUTO-START)
        "CRC-32 encoder (ITU-T V.42) - batch processing"  // description
    };
    
    run_manual_block(&pipeIn, &pipeOut, config, process_crc_encode, init_crc_encode);
    
    return 0;
}
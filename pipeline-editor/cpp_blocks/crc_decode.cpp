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

struct CrcDecodeData {
    std::vector<uint32_t> crcTable;
    int errorCount;
    int packetCount;
};

CrcDecodeData init_crc_decode(const BlockConfig& config) {
    CrcDecodeData data;
    data.crcTable = build_crc32_table();
    data.errorCount = 0;
    data.packetCount = 0;
    return data;
}

void process_crc_decode(
    const char** pipeIn,
    const char** pipeOut,
    CrcDecodeData& customData,
    const BlockConfig& config
) {
    // Create I/O handlers
    PipeIO input(pipeIn[0], config.inputPacketSizes[0], config.inputBatchSizes[0]);
    PipeIO output(pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);
    
    // Allocate buffers
    int8_t* inputBatch = new int8_t[input.getBufferSize()];
    int8_t* outputBatch = new int8_t[output.getBufferSize()];
    
    // MANUAL READ
    int actualCount = input.read(inputBatch);

    int inputPacketSize = input.getPacketSize();
    int outputPacketSize = output.getPacketSize();
    int dataSize = inputPacketSize - 4;

    static bool firstBatch = true;
    if (firstBatch) {
        firstBatch = false;
        printf("[CrcDecode] pkt[0] INPUT : %d bits (data=%d + CRC=32)\n",
               inputPacketSize * 8, dataSize * 8);
        printf("[CrcDecode] pkt[0] OUTPUT: %d bits (data) + 8 bits (error flag)\n",
               dataSize * 8);
        fflush(stdout);
    }
    
    memset(outputBatch, 0, output.getBufferSize());
    
    // Process each packet
    for (int i = 0; i < actualCount; i++) {
        int inputOffset = i * inputPacketSize;
        int outputOffset = i * outputPacketSize;
        
        int8_t* dataInt8 = inputBatch + inputOffset;
        int8_t* crcInt8 = inputBatch + inputOffset + dataSize;
        
        uint8_t* dataAsUint8 = (uint8_t*)dataInt8;
        uint32_t calculatedCrc = calculate_crc32(dataAsUint8, dataSize, customData.crcTable);
        
        uint8_t crcBytes[4];
        for (int j = 0; j < 4; j++) {
            crcBytes[j] = (uint8_t)((int32_t)crcInt8[j] + 128);
        }
        uint32_t receivedCrc = *(uint32_t*)crcBytes;
        
        int8_t errorFlag = (receivedCrc == calculatedCrc) ? 0 : 1;
        if (errorFlag) {
            customData.errorCount++;
        }
        
        memcpy(outputBatch + outputOffset, dataInt8, dataSize);
        outputBatch[outputOffset + outputPacketSize - 1] = errorFlag;
        
        customData.packetCount++;
    }
    
    // MANUAL WRITE
    output.write(outputBatch, actualCount);
    

    
    delete[] inputBatch;
    delete[] outputBatch;
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
        {64},            // inputBatchSizes
        {1501},             // outputPacketSizes
        {64},            // outputBatchSizes
        true,               // ltr
        true,               // startWithAll (AUTO-START)
        "CRC-32 decoder with error detection - batch processing"  // description
    };
    
    run_manual_block(&pipeIn, &pipeOut, config, process_crc_decode, init_crc_decode);
    
    return 0;
}
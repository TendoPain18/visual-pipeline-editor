#include "core/run_generic_block.h"
#include <cstring>
#include <cstdint>
#include <cstdio>

// ============================================================
// QAM Bypass (Frequency Domain Preamble Stripper)
//
// Bypasses batch_ifft → preamble_stripper → batch_fft by
// stripping the 4 preamble blocks directly in the frequency
// domain.
//
// Inputs:
//   in[0]: Freq-domain IQ from qam_mapper   (130048 bytes/pkt)
//             508 blocks x 256 bytes
//             Blocks 0-1: STS
//             Blocks 2-3: LTS
//             Block  4:   SIGNAL
//             Blocks 5+:  DATA
//
// Outputs:
//   out[0]: Freq-domain IQ for qam_demapper (129024 bytes/pkt)
//             504 blocks x 256 bytes
//             Block  0:   SIGNAL
//             Blocks 1+:  DATA
//
// The last 4 bytes of each packet carry the actual block count
// (written by qam_mapper). This block reads that count, subtracts
// 4 (preamble), and writes the updated count into the output.
// ============================================================

static const int PREAMBLE_BLOCKS   = 4;
static const int SUBCARRIERS       = 64;
static const int BYTES_PER_BLOCK   = SUBCARRIERS * 4;                        // 256
static const int IN_TOTAL_BLOCKS   = 508;
static const int OUT_TOTAL_BLOCKS  = IN_TOTAL_BLOCKS - PREAMBLE_BLOCKS;      // 504
static const int IN_PKT_SIZE       = IN_TOTAL_BLOCKS  * BYTES_PER_BLOCK;     // 130048
static const int OUT_PKT_SIZE      = OUT_TOTAL_BLOCKS * BYTES_PER_BLOCK;     // 129024
static const int PREAMBLE_BYTES    = PREAMBLE_BLOCKS  * BYTES_PER_BLOCK;     // 1024

struct QamBypassData { int frameCount; };

QamBypassData init_qam_bypass(const BlockConfig& config) {
    QamBypassData data;
    data.frameCount = 0;
    return data;
}

void process_qam_bypass(
    const char**    pipeIn,
    const char**    pipeOut,
    QamBypassData&  customData,
    const BlockConfig& config
) {
    PipeIO inIQ (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);
    PipeIO outIQ(pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);

    int8_t* inBuf  = new int8_t[inIQ.getBufferSize()];
    int8_t* outBuf = new int8_t[outIQ.getBufferSize()];

    const int inPkt  = config.inputPacketSizes[0];   // 130048
    const int outPkt = config.outputPacketSizes[0];  // 129024

    const bool isFirstBatch = (customData.frameCount == 0);

    int actualCount = inIQ.read(inBuf);

    memset(outBuf, 0x80, outIQ.getBufferSize());

    for (int i = 0; i < actualCount; i++) {
        const int8_t* pktIn  = inBuf  + i * inPkt;
        int8_t*       pktOut = outBuf + i * outPkt;

        // Strip preamble: skip first 4 blocks, copy remaining 504 blocks
        memcpy(pktOut, pktIn + PREAMBLE_BYTES, OUT_TOTAL_BLOCKS * BYTES_PER_BLOCK);

        // Read actual block count embedded in last 4 bytes by qam_mapper
        uint32_t actualIn = 0;
        for (int j = 0; j < 4; j++)
            actualIn |= ((uint32_t)(uint8_t)((int32_t)pktIn[inPkt - 4 + j] + 128)) << (j * 8);
        if (actualIn == 0 || actualIn > (uint32_t)IN_TOTAL_BLOCKS)
            actualIn = IN_TOTAL_BLOCKS;

        uint32_t actualOut = (actualIn > (uint32_t)PREAMBLE_BLOCKS)
                             ? actualIn - PREAMBLE_BLOCKS : 0;

        // Write updated actual block count into last 4 bytes of output
        for (int j = 0; j < 4; j++) {
            uint8_t b = (uint8_t)((actualOut >> (j * 8)) & 0xFF);
            pktOut[outPkt - 4 + j] = (int8_t)((int32_t)b - 128);
        }

        if (isFirstBatch && i == 0) {
            printf("[QamBypass] pkt[0] INPUT : %d bits (%u freq blocks x 256 bytes)\n",
                   (int)actualIn * BYTES_PER_BLOCK * 8, actualIn);
            printf("[QamBypass] pkt[0] OUTPUT: %d bits (%u freq blocks x 256 bytes)\n",
                   (int)actualOut * BYTES_PER_BLOCK * 8, actualOut);
            fflush(stdout);
        }
    }

    outIQ.write(outBuf, actualCount);
    customData.frameCount += actualCount;

    delete[] inBuf;
    delete[] outBuf;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: qam_bypass <pipeInIQ> <pipeOutIQ>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1]};
    const char* pipeOuts[] = {argv[2]};

    BlockConfig config = {
        "QamBypass",
        1,             // inputs
        1,             // outputs
        {130048},      // inputPacketSizes  [508 freq blocks * 256 bytes from qam_mapper]
        {64},          // inputBatchSizes
        {129024},      // outputPacketSizes [504 freq blocks * 256 bytes to qam_demapper]
        {64},          // outputBatchSizes
        false,         // ltr
        true,          // startWithAll
        "Frequency-domain preamble stripper: removes 4 preamble blocks (2xSTS + 2xLTS), "
        "bypassing batch_ifft → preamble_stripper → batch_fft"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_qam_bypass, init_qam_bypass);
    return 0;
}
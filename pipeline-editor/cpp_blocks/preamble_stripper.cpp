#include "core/run_generic_block.h"
#include <cstring>
#include <cstdint>
#include <cstdio>

// ============================================================
// IEEE 802.11a Preamble Stripper (Time Domain)
//
// Removes the 4 preamble symbols (2xSTS + 2xLTS) from each
// time-domain packet, leaving SIGNAL + DATA symbols only.
//
// Inputs:
//   in[0]: Stacked time-domain samples (162560 bytes/pkt)
//             508 symbols x 320 bytes each (80 samples x 4 bytes)
//
// Outputs:
//   out[0]: Stripped time-domain samples (161280 bytes/pkt)
//             504 symbols x 320 bytes each
//             = (508 - 4) * 320
// ============================================================

static const int PREAMBLE_SYMBOLS  = 4;
static const int SAMPLES_PER_SYM   = 80;
static const int BYTES_PER_SAMPLE  = 4;
static const int BYTES_PER_SYM     = SAMPLES_PER_SYM * BYTES_PER_SAMPLE;  // 320
static const int PREAMBLE_BYTES    = PREAMBLE_SYMBOLS * BYTES_PER_SYM;    // 1280
static const int IN_TOTAL_SYMBOLS  = 508;
static const int OUT_TOTAL_SYMBOLS = IN_TOTAL_SYMBOLS - PREAMBLE_SYMBOLS;  // 504
static const int IN_PKT_SIZE       = IN_TOTAL_SYMBOLS  * BYTES_PER_SYM;   // 162560
static const int OUT_PKT_SIZE      = OUT_TOTAL_SYMBOLS * BYTES_PER_SYM;   // 161280
static const int MIN_PKT_BYTES     = (PREAMBLE_SYMBOLS + 1) * BYTES_PER_SYM; // 1600

struct PreambleStripperData { int frameCount; };

PreambleStripperData init_preamble_stripper(const BlockConfig& config) {
    PreambleStripperData data;
    data.frameCount = 0;
    return data;
}

void process_preamble_stripper(
    const char**           pipeIn,
    const char**           pipeOut,
    PreambleStripperData&  customData,
    const BlockConfig&     config
) {
    PipeIO inTime (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);
    PipeIO outTime(pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);

    int8_t* inBuf  = new int8_t[inTime.getBufferSize()];
    int8_t* outBuf = new int8_t[outTime.getBufferSize()];

    const int inPkt  = config.inputPacketSizes[0];   // 162560
    const int outPkt = config.outputPacketSizes[0];  // 161280

    const bool isFirstBatch = (customData.frameCount == 0);

    int actualCount = inTime.read(inBuf);

    memset(outBuf, 0x80, outTime.getBufferSize());



    for (int i = 0; i < actualCount; i++) {
        const int8_t* pktIn  = inBuf  + i * inPkt;
        int8_t*       pktOut = outBuf + i * outPkt;

        // Safety check
        if (inPkt < MIN_PKT_BYTES) {
            fprintf(stderr,
                "[PreambleStripper] FATAL pkt[%d]: size %d < minimum %d bytes\n",
                i, inPkt, MIN_PKT_BYTES);
            exit(1);
        }

        // Strip preamble: skip first PREAMBLE_BYTES, copy the rest
        memcpy(pktOut, pktIn + PREAMBLE_BYTES, outPkt);

        // Read actual symbol count embedded in last 4 bytes of input
        uint32_t actualIn = 0;
        for (int j = 0; j < 4; j++)
            actualIn |= ((uint32_t)(uint8_t)((int32_t)pktIn[inPkt - 4 + j] + 128)) << (j * 8);
        if (actualIn == 0 || actualIn > (uint32_t)IN_TOTAL_SYMBOLS)
            actualIn = IN_TOTAL_SYMBOLS;

        uint32_t actualOut = (actualIn > (uint32_t)PREAMBLE_SYMBOLS)
                             ? actualIn - PREAMBLE_SYMBOLS : 0;

        // Pass actual output count forward in last 4 bytes of output
        for (int j = 0; j < 4; j++) {
            uint8_t b = (uint8_t)((actualOut >> (j * 8)) & 0xFF);
            pktOut[outPkt - 4 + j] = (int8_t)((int32_t)b - 128);
        }

        if (isFirstBatch && i == 0) {
            printf("[PreambleStripper] pkt[0] INPUT : %d bits (%u syms x %d bytes)\n",
                   (int)actualIn * BYTES_PER_SYM * 8, actualIn, BYTES_PER_SYM);
            printf("[PreambleStripper] pkt[0] OUTPUT: %d bits (%u syms x %d bytes)\n",
                   (int)actualOut * BYTES_PER_SYM * 8, actualOut, BYTES_PER_SYM);
            fflush(stdout);
        }
    }



    outTime.write(outBuf, actualCount);
    customData.frameCount += actualCount;

    delete[] inBuf;
    delete[] outBuf;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: preamble_stripper <pipeInTime> <pipeOutTime>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1]};
    const char* pipeOuts[] = {argv[2]};

    BlockConfig config = {
        "PreambleStripper",
        1,           // inputs
        1,           // outputs
        {162560},    // inputPacketSizes  [508 symbols * 320 bytes]
        {64},        // inputBatchSizes
        {161280},    // outputPacketSizes [504 symbols * 320 bytes]
        {64},        // outputBatchSizes
        false,       // ltr
        true,        // startWithAll
        "IEEE 802.11a Preamble Stripper: removes 4 preamble symbols (2xSTS + 2xLTS) "
        "from time-domain packet, passing SIGNAL+DATA symbols downstream"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_preamble_stripper, init_preamble_stripper);
    return 0;
}
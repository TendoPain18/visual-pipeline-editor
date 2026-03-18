#include "core/run_generic_block.h"
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cstdio>

// ============================================================
// IEEE 802.11a Preamble Stripper (Time Domain)
//
// Sits between batch_ifft and batch_fft.
// Removes the 4 preamble symbols (STS x2, LTS x2) from each
// time-domain packet, leaving SIGNAL + DATA symbols only.
//
// Inputs:
//   in[0]: Stacked time-domain samples (162560 bytes/pkt)
//             508 symbols x 320 bytes each (80 samples x 4 bytes)
//
// Outputs:
//   out[0]: Stripped time-domain samples (161280 bytes/pkt)
//             504 symbols x 320 bytes each (80 samples x 4 bytes)
//             = (508 - 4) * 320
//
// Stripping logic:
//   - Each symbol (after IFFT+CP) = 80 samples = 320 bytes
//   - Preamble occupies first 4 symbols = first 1280 bytes
//   - Fatal error if packet has fewer than 5 symbols (4 preamble + 1 SIGNAL)
//
// int8 pipe convention: stored byte B -> pipe int8_t = int8_t(uint8_t(B) - 128)
// ============================================================

// -----------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------
static const int PREAMBLE_SYMBOLS  = 4;                                    // 2 STS + 2 LTS
static const int SAMPLES_PER_SYM   = 80;                                   // 64 IFFT + 16 CP
static const int BYTES_PER_SAMPLE  = 4;
static const int BYTES_PER_SYM     = SAMPLES_PER_SYM * BYTES_PER_SAMPLE;  // 320
static const int PREAMBLE_BYTES    = PREAMBLE_SYMBOLS * BYTES_PER_SYM;    // 1280
static const int IN_TOTAL_SYMBOLS  = 508;
static const int OUT_TOTAL_SYMBOLS = IN_TOTAL_SYMBOLS - PREAMBLE_SYMBOLS;  // 504
static const int IN_PKT_SIZE       = IN_TOTAL_SYMBOLS  * BYTES_PER_SYM;   // 162560
static const int OUT_PKT_SIZE      = OUT_TOTAL_SYMBOLS * BYTES_PER_SYM;   // 161280
static const int MIN_PKT_BYTES     = (PREAMBLE_SYMBOLS + 1) * BYTES_PER_SYM; // 1600
static const int BATCH_SIZE        = 64;

// -----------------------------------------------------------------------
// Block state
// -----------------------------------------------------------------------
struct PreambleStripperData {
    int frameCount;
};

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

    const int inPkt  = config.inputPacketSizes[0];
    const int outPkt = config.outputPacketSizes[0];

    int actualCount = inTime.read(inBuf);

    memset(outBuf, 0x80, outTime.getBufferSize());

    for (int i = 0; i < actualCount; i++) {
        const int8_t* pktIn  = inBuf  + i * inPkt;
        int8_t*       pktOut = outBuf + i * outPkt;

        // Safety check: packet must contain preamble + at least one SIGNAL symbol
        if (inPkt < MIN_PKT_BYTES) {
            fprintf(stderr,
                "[PreambleStripper] FATAL pkt[%d]: packet size %d bytes is smaller than "
                "minimum required %d bytes (%d preamble symbols + 1 SIGNAL symbol).\n",
                i, inPkt, MIN_PKT_BYTES, PREAMBLE_SYMBOLS);
            exit(1);
        }

        // Strip preamble: copy everything after the first PREAMBLE_BYTES bytes
        memcpy(pktOut, pktIn + PREAMBLE_BYTES, outPkt);
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
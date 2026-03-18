#include "core/run_generic_block.h"
#include <cstring>

// ============================================================
// Scrambler Middleman  (test/bypass block for scrambler layer)
//
// Sits between scrambler and descrambler for isolated testing.
// Will be removed once the scrambler layer is verified.
//
// Inputs:
//   in[0]: DATA pipe from scrambler             (1515 bytes/pkt)  <- FIRST
//   in[1]: RATE/LIP pipe                         (5 bytes/pkt)
//             Byte 0: rate_value
//             Bytes 1-4: lipBits uint32 LE (EXACT bit count)
//   in[2]: Feedback from descrambler             (3 bytes/pkt)
//
// Outputs:
//   out[0]: lip + DATA -> descrambler            (1519 bytes/pkt)
//             Bytes 0-3: lipBits uint32 LE (from in[1])
//             Bytes 4-1518: DATA (full 1515 bytes)
//   out[1]: SIGNAL -> descrambler                (3 bytes/pkt)
//
// Protocol per batch:
//   1. Read DATA batch        (in[0])  -- FIRST (rate measurement)
//   2. Read RATE/LIP batch    (in[1])  -- extract lipBits uint32 LE
//   3. Extract SIGNAL (first 3 bytes of each DATA packet)
//   4. Send SIGNAL batch      (out[1]) -- SIGNAL goes FIRST
//   5. Wait for feedback      (in[2])  -- gate; discard content
//   6. Fill header [lipBits(4B LE)], send out[0]
// ============================================================

static const int DEBUG_PKT_LIMIT = 3;

struct ScramblerMiddlemanData {
    int frameCount;
};

ScramblerMiddlemanData init_scrambler_middleman(const BlockConfig& config) {
    ScramblerMiddlemanData data;
    data.frameCount = 0;

    printf("[ScramblerMiddleman] Staged I/O mode (CORRECTED)\n");
    printf("[ScramblerMiddleman] in[0]: DATA (1515 bytes/pkt)\n");
    printf("[ScramblerMiddleman] in[1]: RATE/LIP (5 bytes/pkt) = [rate_val(1B) | lipBits uint32 LE(4B)]\n");
    printf("[ScramblerMiddleman] in[2]: Feedback (3 bytes/pkt)\n");
    printf("[ScramblerMiddleman] out[0] layout: [lipBits(4B LE)|DATA(1515)] = 1519 bytes/pkt\n");
    printf("[ScramblerMiddleman] out[1] layout: SIGNAL (3 bytes/pkt)\n");
    printf("[ScramblerMiddleman] Sequence per batch:\n");
    printf("[ScramblerMiddleman]   1. Read DATA batch    (in[0], 1515 bytes/pkt) -- FIRST\n");
    printf("[ScramblerMiddleman]   2. Read RATE/LIP      (in[1], 5 bytes/pkt)    -- extract lipBits\n");
    printf("[ScramblerMiddleman]   3. Send SIGNAL batch  (out[1], 3 bytes/pkt)   -- SIGNAL first\n");
    printf("[ScramblerMiddleman]   4. Wait for feedback  (in[2], 3 bytes/pkt)    -- gate\n");
    printf("[ScramblerMiddleman]   5. Send lip+DATA      (out[0], 1519 bytes/pkt)-- DATA second\n");
    printf("[ScramblerMiddleman] Ready\n");

    return data;
}

void process_scrambler_middleman(
    const char** pipeIn,
    const char** pipeOut,
    ScramblerMiddlemanData& customData,
    const BlockConfig& config
) {
    PipeIO inData    (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);  // 1515
    PipeIO inRate    (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);  // 5
    PipeIO inFeedback(pipeIn[2],  config.inputPacketSizes[2],  config.inputBatchSizes[2]);  // 3
    PipeIO outData   (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]); // 1519
    PipeIO outSignal (pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]); // 3

    int8_t* dataBuf     = new int8_t[inData.getBufferSize()];
    int8_t* rateBuf     = new int8_t[inRate.getBufferSize()];
    int8_t* feedbackBuf = new int8_t[inFeedback.getBufferSize()];
    int8_t* dataOutBuf  = new int8_t[outData.getBufferSize()];
    int8_t* signalBuf   = new int8_t[outSignal.getBufferSize()];

    const int inDataPkt  = config.inputPacketSizes[0];   // 1515
    const int inRatePkt  = config.inputPacketSizes[1];   // 5 (rate_val + lipBits uint32 LE)
    const int outDataPkt = config.outputPacketSizes[0];  // 1519 (lipBits + DATA)
    const int outSigPkt  = config.outputPacketSizes[1];  // 3

    const bool isFirstBatch = (customData.frameCount == 0);

    // ===== STEP 1: Read DATA batch first (rate measurement) =====
    int actualCount = inData.read(dataBuf);

    // ===== STEP 2: Read RATE/LIP batch -- extract lipBits uint32 LE, discard rate_val =====
    inRate.read(rateBuf);

    memset(dataOutBuf, 0, outData.getBufferSize());
    memset(signalBuf,  0, outSignal.getBufferSize());

    // Store lipBits per packet before feedback arrives
    uint32_t* lipBitsArr = new uint32_t[actualCount];

    // ===== STEP 3: Extract SIGNAL (first 3B) and lipBits; store for later =====
    for (int i = 0; i < actualCount; i++) {
        const int inOff   = i * inDataPkt;
        const int rateOff = i * inRatePkt;
        const int sigOff  = i * outSigPkt;
        const int dataOff = i * outDataPkt;

        // Extract lipBits from RATE/LIP pipe (bytes 1-4 as uint32 LE)
        uint8_t b0 = (uint8_t)((int32_t)rateBuf[rateOff + 1] + 128);
        uint8_t b1 = (uint8_t)((int32_t)rateBuf[rateOff + 2] + 128);
        uint8_t b2 = (uint8_t)((int32_t)rateBuf[rateOff + 3] + 128);
        uint8_t b3 = (uint8_t)((int32_t)rateBuf[rateOff + 4] + 128);
        uint32_t lipBits = (uint32_t)b0 | ((uint32_t)b1 << 8)
                         | ((uint32_t)b2 << 16) | ((uint32_t)b3 << 24);
        lipBitsArr[i] = lipBits;

        // First 3 bytes of DATA = SIGNAL
        memcpy(signalBuf + sigOff, dataBuf + inOff, 3);

        // Copy full 1515-byte DATA packet -> out[0] bytes [4..1518]
        // (bytes 0-3 will be filled with lipBits later)
        memcpy(dataOutBuf + dataOff + 4, dataBuf + inOff, inDataPkt);

        if (isFirstBatch && i < DEBUG_PKT_LIMIT) {
            fprintf(stderr, "[ScramblerMiddleman] DBG pkt[%d] lipBits=0x%08X (%u bits)\n",
                    i, (unsigned)lipBits, (unsigned)lipBits);
        }
    }

    // ===== STEP 4: Send SIGNAL batch FIRST =====
    outSignal.write(signalBuf, actualCount);

    // ===== STEP 5: Wait for feedback (gate) -- discard content =====
    inFeedback.read(feedbackBuf);

    // ===== STEP 6: Fill header [lipBits uint32 LE], send out[0] =====
    for (int i = 0; i < actualCount; i++) {
        const int dataOff = i * outDataPkt;
        uint32_t lipBits = lipBitsArr[i];

        // Pack lipBits as uint32 LE into first 4 bytes
        dataOutBuf[dataOff + 0] = (int8_t)((int32_t)((lipBits      ) & 0xFF) - 128);
        dataOutBuf[dataOff + 1] = (int8_t)((int32_t)((lipBits >>  8) & 0xFF) - 128);
        dataOutBuf[dataOff + 2] = (int8_t)((int32_t)((lipBits >> 16) & 0xFF) - 128);
        dataOutBuf[dataOff + 3] = (int8_t)((int32_t)((lipBits >> 24) & 0xFF) - 128);

        if (isFirstBatch && i < DEBUG_PKT_LIMIT) {
            fprintf(stderr, "[ScramblerMiddleman] DBG pkt[%d] lipBits=0x%08X -> header set\n",
                    i, (unsigned)lipBits);
        }
    }

    outData.write(dataOutBuf, actualCount);

    customData.frameCount += actualCount;

    delete[] dataBuf;
    delete[] rateBuf;
    delete[] feedbackBuf;
    delete[] dataOutBuf;
    delete[] signalBuf;
    delete[] lipBitsArr;
}

int main(int argc, char* argv[]) {
    if (argc < 6) {
        fprintf(stderr,
            "Usage: scrambler_middleman <pipeInData> <pipeInRate> <pipeInFeedback>"
            " <pipeOutData> <pipeOutSignal>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1], argv[2], argv[3]};
    const char* pipeOuts[] = {argv[4], argv[5]};

    BlockConfig config = {
        "ScramblerMiddleman",
        3,                          // inputs
        2,                          // outputs
        {1515, 5, 3},               // inputPacketSizes  [DATA, RATE/LIP(5), feedback]
        {64, 64, 64},               // inputBatchSizes
        {1519, 3},                  // outputPacketSizes [lipBits(4)+DATA(1515), SIGNAL(3)]
        {64, 64},                   // outputBatchSizes
        false,                      // ltr (middleman)
        true,                       // startWithAll
        "Scrambler middleman: lipBits uint32 LE (4B) + DATA (1515B) = 1519B"
    };

    run_manual_block(pipeIns, pipeOuts, config,
                     process_scrambler_middleman, init_scrambler_middleman);
    return 0;
}
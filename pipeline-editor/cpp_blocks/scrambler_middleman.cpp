#include "core/run_generic_block.h"
#include <cstring>

// ============================================================
// Scrambler Middleman  (test/bypass block for scrambler layer)
//
// Sits between scrambler and descrambler for isolated testing.
// Will be removed once the scrambler layer is verified.
//
// Inputs:
//   in[0]: DATA pipe from scrambler   (1515 bytes/pkt, max)  <- FIRST
//   in[1]: RATE/LIP pipe              (3 bytes/pkt)
//             Byte 0: rate_value  (unused; no rate forwarded here)
//             Byte 1: lip_lo
//             Byte 2: lip_hi
//   in[2]: Feedback from ppdu_decap   (3 bytes/pkt)
//
// Outputs:
//   out[0]: lip + DATA -> descrambler  (1518 bytes/pkt)  <- FIRST
//             Byte 0: lip_lo   (from in[1])
//             Byte 1: lip_hi   (from in[1])
//             Bytes [2..1516]: DATA (full 1515 bytes)
//   out[1]: SIGNAL -> descrambler      (3 bytes/pkt)
//
// Protocol per batch:
//   1. Read DATA batch      (in[0])  -- FIRST (rate measurement)
//   2. Read RATE/LIP batch  (in[1])  -- extract lip; discard rate
//   3. Extract SIGNAL (first 3 bytes of each packet)
//   4. Send SIGNAL batch    (out[1]) -- SIGNAL goes FIRST
//   5. Wait for feedback    (in[2])  -- gate; discard content
//   6. Fill header [lip_lo | lip_hi], send out[0]
// ============================================================

static const int DEBUG_PKT_LIMIT = 3;

struct ScramblerMiddlemanData {
    int frameCount;
};

ScramblerMiddlemanData init_scrambler_middleman(const BlockConfig& config) {
    ScramblerMiddlemanData data;
    data.frameCount = 0;

    printf("[ScramblerMiddleman] Staged I/O mode\n");
    printf("[ScramblerMiddleman] out[0] layout: [lip_lo(1)|lip_hi(1)|DATA(1515)]\n");
    printf("[ScramblerMiddleman] Sequence per batch:\n");
    printf("[ScramblerMiddleman]   1. Read DATA batch    (in[0], 1515 bytes/pkt) -- FIRST\n");
    printf("[ScramblerMiddleman]   2. Read RATE/LIP      (in[1], 3 bytes/pkt)   -- lip only\n");
    printf("[ScramblerMiddleman]   3. Send SIGNAL batch  (out[1], 3 bytes/pkt)  -- SIGNAL first\n");
    printf("[ScramblerMiddleman]   4. Wait for feedback  (in[2], 3 bytes/pkt)   -- gate\n");
    printf("[ScramblerMiddleman]   5. Send lip+DATA      (out[0], 1517 bytes/pkt)-- DATA second\n");
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
    PipeIO inRate    (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);  // 3
    PipeIO inFeedback(pipeIn[2],  config.inputPacketSizes[2],  config.inputBatchSizes[2]);  // 3
    PipeIO outData   (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]); // 1517
    PipeIO outSignal (pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]); // 3

    int8_t* dataBuf     = new int8_t[inData.getBufferSize()];
    int8_t* rateBuf     = new int8_t[inRate.getBufferSize()];
    int8_t* feedbackBuf = new int8_t[inFeedback.getBufferSize()];
    int8_t* dataOutBuf  = new int8_t[outData.getBufferSize()];
    int8_t* signalBuf   = new int8_t[outSignal.getBufferSize()];

    const int inDataPkt  = config.inputPacketSizes[0];   // 1515
    const int inRatePkt  = config.inputPacketSizes[1];   // 3
    const int outDataPkt = config.outputPacketSizes[0];  // 1517
    const int outSigPkt  = config.outputPacketSizes[1];  // 3

    const bool isFirstBatch = (customData.frameCount == 0);

    // ===== STEP 1: Read DATA batch first (rate measurement) =====
    int actualCount = inData.read(dataBuf);

    // ===== STEP 2: Read RATE/LIP batch -- extract lip, discard rate =====
    inRate.read(rateBuf);

    memset(dataOutBuf, 0, outData.getBufferSize());
    memset(signalBuf,  0, outSignal.getBufferSize());

    // Store lip per packet before feedback arrives
    int* lipArr = new int[actualCount];

    // ===== STEP 3: Extract SIGNAL (first 3B) and DATA; store lip =====
    for (int i = 0; i < actualCount; i++) {
        const int inOff   = i * inDataPkt;
        const int rateOff = i * inRatePkt;
        const int sigOff  = i * outSigPkt;
        const int dataOff = i * outDataPkt;

        // Grab lip from RATE/LIP pipe (bytes 1+2)
        uint8_t lipLo = (uint8_t)((int32_t)rateBuf[rateOff + 1] + 128);
        uint8_t lipHi = (uint8_t)((int32_t)rateBuf[rateOff + 2] + 128);
        int lip = (int)lipLo | ((int)lipHi << 8);
        lipArr[i] = lip;

        // First 3 bytes = SIGNAL
        memcpy(signalBuf + sigOff, dataBuf + inOff, 3);

        // Full 1515-byte packet -> out[0] bytes [2..] (header TBD)
        int copyLen = inDataPkt;
        if (copyLen > outDataPkt - 2) copyLen = outDataPkt - 2;
        memcpy(dataOutBuf + dataOff + 2, dataBuf + inOff, copyLen);

        if (isFirstBatch && i < DEBUG_PKT_LIMIT) {
            printf("[ScramblerMiddleman] DBG pkt[%d] lip=%d\n", i, lip);
        }
    }

    // ===== STEP 4: Send SIGNAL batch FIRST =====
    outSignal.write(signalBuf, actualCount);

    // ===== STEP 5: Wait for feedback (gate) -- discard content =====
    inFeedback.read(feedbackBuf);

    // ===== STEP 6: Fill header [lip_lo | lip_hi], send out[0] =====
    for (int i = 0; i < actualCount; i++) {
        const int dataOff = i * outDataPkt;
        int lip = lipArr[i];

        dataOutBuf[dataOff + 0] = (int8_t)((int32_t)(lip & 0xFF)        - 128);
        dataOutBuf[dataOff + 1] = (int8_t)((int32_t)((lip >> 8) & 0xFF) - 128);

        if (isFirstBatch && i < DEBUG_PKT_LIMIT) {
            printf("[ScramblerMiddleman] DBG pkt[%d] lip=%d -> header set\n", i, lip);
        }
    }

    outData.write(dataOutBuf, actualCount);

    customData.frameCount += actualCount;

    delete[] dataBuf;
    delete[] rateBuf;
    delete[] feedbackBuf;
    delete[] dataOutBuf;
    delete[] signalBuf;
    delete[] lipArr;
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
        {1515, 3, 3},               // inputPacketSizes  [DATA, RATE/LIP, feedback]
        {64, 64, 64},         // inputBatchSizes
        {1517, 3},                  // outputPacketSizes [lip+DATA(1517), SIGNAL(3)]
        {64, 64},               // outputBatchSizes
        false,                      // ltr (middleman)
        true,                       // startWithAll
        "Scrambler test middleman: SIGNAL first, gate on feedback, lip+DATA second"
    };

    run_manual_block(pipeIns, pipeOuts, config,
                     process_scrambler_middleman, init_scrambler_middleman);
    return 0;
}
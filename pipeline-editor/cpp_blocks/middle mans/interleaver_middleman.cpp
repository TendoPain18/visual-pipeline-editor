#include "core/run_generic_block.h"
#include <cstring>

// ============================================================
// Interleaver Middleman
//
// Inputs:
//   in[0]: Interleaved DATA from interleaver     (3030 bytes/pkt)  <- FIRST
//   in[1]: rate_encoded_length from interleaver  (5 bytes/pkt)
//             Byte 0:   rate_value
//             Bytes 1-4: encoded_len_bits (uint32 LE) -- EXACT encoded bit count
//   in[2]: Feedback from ppdu_decapsulate        (3 bytes/pkt)
//             Byte 0: rate_value
//             Bytes 1-2: unused
//
// Outputs:
//   out[0]: rate + lip_bits + DATA_INTERLEAVED   (3029 bytes/pkt)  <- SECOND
//             Byte 0:    rate_value   (from feedback)
//             Bytes 1-4: lip_bits     (uint32 LE) = encLenBits - 48
//                        (exact DATA field bit count, excluding SIGNAL 48 bits)
//             Bytes [5..3028]: interleaved DATA (after SIGNAL strip, 3024 bytes)
//   out[1]: SIGNAL_INTERLEAVED -> deinterleaver  (6 bytes/pkt)  <- FIRST
//
// The "lip_bits" here = exact DATA encoded bit count (encLenBits - 48).
// Downstream deinterleaver uses it to bound processing to real data bits only.
//
// Protocol per batch:
//   1. Read interleaved DATA batch   (in[0])  -- FIRST (rate measurement)
//   2. Read rate_encoded_length      (in[1])  -- extract encLenBits (uint32 LE)
//   3. Split: first 6 bytes = SIGNAL_INTERLEAVED -> out[1]
//             remaining bytes -> out[0] bytes [5..]
//   4. Send SIGNAL_INTERLEAVED batch (out[1])
//   5. Wait for feedback             (in[2])  -- gate; extract rate_value
//   6. Fill header bytes [0..4] with rate + lip_bits, send out[0]
// ============================================================

static const int DEBUG_PKT_LIMIT = 3;

struct InterleaverMiddlemanData {
    int frameCount;
};

InterleaverMiddlemanData init_interleaver_middleman(const BlockConfig& config) {
    InterleaverMiddlemanData data;
    data.frameCount = 0;



    return data;
}

void process_interleaver_middleman(
    const char** pipeIn,
    const char** pipeOut,
    InterleaverMiddlemanData& customData,
    const BlockConfig& config
) {
    PipeIO inData    (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);  // 3030
    PipeIO inRate    (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);  // 5
    PipeIO inFeedback(pipeIn[2],  config.inputPacketSizes[2],  config.inputBatchSizes[2]);  // 3
    PipeIO outData   (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]); // 3029
    PipeIO outSignal (pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]); // 6

    int8_t* dataBuf     = new int8_t[inData.getBufferSize()];
    int8_t* rateBuf     = new int8_t[inRate.getBufferSize()];
    int8_t* feedbackBuf = new int8_t[inFeedback.getBufferSize()];
    int8_t* signalBuf   = new int8_t[outSignal.getBufferSize()];
    int8_t* dataOutBuf  = new int8_t[outData.getBufferSize()];

    const int inDataPkt  = config.inputPacketSizes[0];   // 3030
    const int inRatePkt  = config.inputPacketSizes[1];   // 5
    const int inFbPkt    = config.inputPacketSizes[2];   // 3
    const int outDataPkt = config.outputPacketSizes[0];  // 3029
    const int outSigPkt  = config.outputPacketSizes[1];  // 6

    const bool isFirstBatch = (customData.frameCount == 0);

    // ===== STEP 1: Read interleaved DATA batch (rate measurement) =====
    int actualCount = inData.read(dataBuf);

    // ===== STEP 2: Read rate_encoded_length (5 bytes: rate_val + encLenBits uint32 LE) =====
    inRate.read(rateBuf);

    memset(signalBuf,  0, outSignal.getBufferSize());
    memset(dataOutBuf, 0, outData.getBufferSize());

    // Store lip_bits (DATA field exact bit count) per packet before feedback arrives
    uint32_t* lipBitsArr = new uint32_t[actualCount];

    // ===== STEP 3: Split SIGNAL (6B) and DATA_INTERLEAVED; compute lip_bits =====
    for (int i = 0; i < actualCount; i++) {
        const int inOff   = i * inDataPkt;
        const int rateOff = i * inRatePkt;
        const int sigOff  = i * outSigPkt;
        const int dataOff = i * outDataPkt;

        // Decode encLenBits from interleaver rate pipe bytes [1..4] (uint32 LE)
        uint8_t b1 = (uint8_t)((int32_t)rateBuf[rateOff + 1] + 128);
        uint8_t b2 = (uint8_t)((int32_t)rateBuf[rateOff + 2] + 128);
        uint8_t b3 = (uint8_t)((int32_t)rateBuf[rateOff + 3] + 128);
        uint8_t b4 = (uint8_t)((int32_t)rateBuf[rateOff + 4] + 128);
        uint32_t encLenBits = (uint32_t)b1 | ((uint32_t)b2 << 8)
                            | ((uint32_t)b3 << 16) | ((uint32_t)b4 << 24);

        // DATA field bit count = total encoded bits - 48 (SIGNAL field)
        uint32_t dataLipBits = (encLenBits >= 48) ? encLenBits - 48 : 0;
        lipBitsArr[i] = dataLipBits;

        // First 6 bytes of interleaved packet = SIGNAL_INTERLEAVED
        memcpy(signalBuf + sigOff, dataBuf + inOff, 6);

        // Remaining bytes = DATA_INTERLEAVED -> out[0] bytes [5..] (header filled later)
        int copyLen = inDataPkt - 6;   // 3024
        if (copyLen > outDataPkt - 5) copyLen = outDataPkt - 5;
        memcpy(dataOutBuf + dataOff + 5, dataBuf + inOff + 6, copyLen);

        if (isFirstBatch && i == 0) {
            printf("[InterleaverMiddleman] pkt[0] INPUT : signal=48  data=%u  total=%u bits\n",
                   dataLipBits, (unsigned)encLenBits);
        }
    }

    // ===== STEP 4: Send SIGNAL_INTERLEAVED =====
    outSignal.write(signalBuf, actualCount);

    // ===== STEP 5: Wait for feedback (gate) -- extract rate_value =====
    inFeedback.read(feedbackBuf);

    // ===== STEP 6: Fill header [rate(1) | lip_bits(4B LE)], send out[0] =====
    for (int i = 0; i < actualCount; i++) {
        const int fbOff   = i * inFbPkt;
        const int dataOff = i * outDataPkt;

        uint8_t  rateVal    = (uint8_t)((int32_t)feedbackBuf[fbOff + 0] + 128);
        uint32_t dataLipBits = lipBitsArr[i];

        dataOutBuf[dataOff + 0] = (int8_t)((int32_t)rateVal                          - 128);
        dataOutBuf[dataOff + 1] = (int8_t)((int32_t)( dataLipBits        & 0xFF)     - 128);
        dataOutBuf[dataOff + 2] = (int8_t)((int32_t)((dataLipBits >>  8) & 0xFF)     - 128);
        dataOutBuf[dataOff + 3] = (int8_t)((int32_t)((dataLipBits >> 16) & 0xFF)     - 128);
        dataOutBuf[dataOff + 4] = (int8_t)((int32_t)((dataLipBits >> 24) & 0xFF)     - 128);

        if (isFirstBatch && i == 0) {
            printf("[InterleaverMiddleman] pkt[0] OUTPUT: signal=48  data=%u  total=%u bits\n",
                   (unsigned)dataLipBits, (unsigned)(48 + dataLipBits));
            fflush(stdout);
        }
    }

    outData.write(dataOutBuf, actualCount);

    customData.frameCount += actualCount;

    delete[] dataBuf;
    delete[] rateBuf;
    delete[] feedbackBuf;
    delete[] signalBuf;
    delete[] dataOutBuf;
    delete[] lipBitsArr;
}

int main(int argc, char* argv[]) {
    if (argc < 6) {
        fprintf(stderr,
            "Usage: interleaver_middleman <pipeInData> <pipeInRate> <pipeInFeedback>"
            " <pipeOutData> <pipeOutSignal>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1], argv[2], argv[3]};
    const char* pipeOuts[] = {argv[4], argv[5]};

    BlockConfig config = {
        "InterleaverMiddleman",
        3,                          // inputs
        2,                          // outputs
        {3030, 5, 3},               // inputPacketSizes  [INT_DATA(3030), rate_enc_len(5), feedback(3)]
        {64, 64, 64},               // inputBatchSizes
        {3029, 6},                  // outputPacketSizes [rate+lip_bits+DATA_INT(3029), SIGNAL_INT(6)]
        {64, 64},                   // outputBatchSizes
        false,                      // ltr (middleman)
        true,                       // startWithAll
        "Interleaver middleman: SIGNAL_INT first; rate+lip_bits(uint32 LE)+DATA_INT second"
    };

    run_manual_block(pipeIns, pipeOuts, config,
                     process_interleaver_middleman,
                     init_interleaver_middleman);
    return 0;
}
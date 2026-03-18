#include "core/run_generic_block.h"
#include <cstring>

// ============================================================
// Interleaver Middleman
//
// Inputs:
//   in[0]: Interleaved DATA from interleaver     (3030 bytes/pkt)  <- FIRST
//   in[1]: rate_encoded_length from interleaver  (3 bytes/pkt)
//             Byte 0: rate_value  (unused here; rate comes from feedback)
//             Byte 1: enc_len_lo  <- this IS our lip for the interleaved domain
//             Byte 2: enc_len_hi
//   in[2]: Feedback from ppdu_decapsulate        (3 bytes/pkt)
//             Byte 0: rate_value
//             Byte 1: mac_len_lo  (unused -- we have enc_len from interleaver)
//             Byte 2: mac_len_hi  (unused)
//
// Outputs:
//   out[0]: rate + lip + DATA_INTERLEAVED        (3027 bytes/pkt)  <- FIRST
//             Byte 0: rate_value   (from feedback)
//             Byte 1: lip_lo       (= enc_len_lo from interleaver, meaningful encoded bytes)
//             Byte 2: lip_hi       (= enc_len_hi from interleaver)
//             Bytes [3..3026]: interleaved DATA (after SIGNAL strip, 3024 bytes)
//   out[1]: SIGNAL_INTERLEAVED -> deinterleaver  (6 bytes/pkt)
//
// The "lip" here = encoded_length = number of meaningful bytes in the 3030-byte
// encoded pipe. Downstream blocks (deinterleaver, channel_decode) use it to
// bound their processing to actual data only.
//
// Protocol per batch:
//   1. Read interleaved DATA batch   (in[0])  -- FIRST (rate measurement)
//   2. Read rate_encoded_length      (in[1])  -- keep enc_len as lip
//   3. Split: first 6 bytes = SIGNAL_INTERLEAVED, rest -> out[0] bytes [3..]
//   4. Send SIGNAL_INTERLEAVED batch (out[1])
//   5. Wait for feedback             (in[2])  -- gate; extract rate_value
//   6. Fill header bytes [0..2], send out[0]
// ============================================================

static const int DEBUG_PKT_LIMIT = 3;

struct InterleaverMiddlemanData {
    int frameCount;
};

InterleaverMiddlemanData init_interleaver_middleman(const BlockConfig& config) {
    InterleaverMiddlemanData data;
    data.frameCount = 0;

    printf("[InterleaverMiddleman] Staged I/O mode\n");
    printf("[InterleaverMiddleman] out[0] layout: [rate(1)|lip_lo(1)|lip_hi(1)|DATA_INT(3024)]\n");
    printf("[InterleaverMiddleman] lip = enc_len from interleaver (meaningful encoded bytes)\n");
    printf("[InterleaverMiddleman] Sequence per batch:\n");
    printf("[InterleaverMiddleman]   1. Read interleaved DATA (in[0], 3030B) -- FIRST\n");
    printf("[InterleaverMiddleman]   2. Read rate_enc_len     (in[1], 3B)    -- use enc_len as lip\n");
    printf("[InterleaverMiddleman]   3. Send SIGNAL_INT       (out[1], 6B)\n");
    printf("[InterleaverMiddleman]   4. Wait for feedback     (in[2], 3B)    -- gate for rate\n");
    printf("[InterleaverMiddleman]   5. Send rate+lip+DATA_INT (out[0], 3027B)\n");
    printf("[InterleaverMiddleman] Ready\n");

    return data;
}

void process_interleaver_middleman(
    const char** pipeIn,
    const char** pipeOut,
    InterleaverMiddlemanData& customData,
    const BlockConfig& config
) {
    PipeIO inData    (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);  // 3030
    PipeIO inRate    (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);  // 3
    PipeIO inFeedback(pipeIn[2],  config.inputPacketSizes[2],  config.inputBatchSizes[2]);  // 3
    PipeIO outData   (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]); // 3027
    PipeIO outSignal (pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]); // 6

    int8_t* dataBuf     = new int8_t[inData.getBufferSize()];
    int8_t* rateBuf     = new int8_t[inRate.getBufferSize()];
    int8_t* feedbackBuf = new int8_t[inFeedback.getBufferSize()];
    int8_t* signalBuf   = new int8_t[outSignal.getBufferSize()];
    int8_t* dataOutBuf  = new int8_t[outData.getBufferSize()];

    const int inDataPkt  = config.inputPacketSizes[0];   // 3030
    const int inRatePkt  = config.inputPacketSizes[1];   // 3
    const int inFbPkt    = config.inputPacketSizes[2];   // 3
    const int outDataPkt = config.outputPacketSizes[0];  // 3027
    const int outSigPkt  = config.outputPacketSizes[1];  // 6

    const bool isFirstBatch = (customData.frameCount == 0);

    // ===== STEP 1: Read interleaved DATA batch (rate measurement) =====
    int actualCount = inData.read(dataBuf);

    // ===== STEP 2: Read rate_encoded_length -- grab enc_len as lip =====
    inRate.read(rateBuf);

    memset(signalBuf,  0, outSignal.getBufferSize());
    memset(dataOutBuf, 0, outData.getBufferSize());

    // Store lip (enc_len) per packet before feedback arrives
    int* lipArr = new int[actualCount];

    // ===== STEP 3: Split SIGNAL (6B) and DATA_INTERLEAVED; store enc_len =====
    for (int i = 0; i < actualCount; i++) {
        const int inOff   = i * inDataPkt;
        const int rateOff = i * inRatePkt;
        const int sigOff  = i * outSigPkt;
        const int dataOff = i * outDataPkt;

        // Grab enc_len from interleaver's rate pipe (bytes 1+2)
        uint8_t encLo = (uint8_t)((int32_t)rateBuf[rateOff + 1] + 128);
        uint8_t encHi = (uint8_t)((int32_t)rateBuf[rateOff + 2] + 128);
        int lip = (int)encLo | ((int)encHi << 8);
        // The interleaved DATA we carry is after stripping the 6-byte SIGNAL,
        // so the DATA portion lip = lip - 6 (in bytes), but we preserve the
        // full enc_len value as the "lip" token so deinterleaver can apply it
        // over the DATA bytes starting at out[0][3..].
        // NOTE: The SIGNAL field (6 bytes in encoded domain) is sent separately
        // via out[1], so we need the DATA portion only = enc_len - 6.
        int dataLip = lip - 6;
        if (dataLip < 0) dataLip = 0;
        lipArr[i] = dataLip;  // DATA encoded bytes (excludes SIGNAL 6B)

        // First 6 bytes of interleaved packet = SIGNAL_INTERLEAVED
        memcpy(signalBuf + sigOff, dataBuf + inOff, 6);

        // Remaining bytes = DATA_INTERLEAVED -> out[0] bytes [3..] (header TBD)
        int copyLen = inDataPkt - 6;   // 3024
        if (copyLen > outDataPkt - 3) copyLen = outDataPkt - 3;
        memcpy(dataOutBuf + dataOff + 3, dataBuf + inOff + 6, copyLen);

        if (isFirstBatch && i < DEBUG_PKT_LIMIT) {
            printf("[InterleaverMiddleman] DBG pkt[%d] enc_len=%d dataLip=%d\n",
                   i, lip, dataLip);
        }
    }

    // ===== STEP 4: Send SIGNAL_INTERLEAVED =====
    outSignal.write(signalBuf, actualCount);

    // ===== STEP 5: Wait for feedback (gate) -- just to get rate_value =====
    inFeedback.read(feedbackBuf);

    // ===== STEP 6: Fill header [rate | lip_lo | lip_hi], send out[0] =====
    for (int i = 0; i < actualCount; i++) {
        const int fbOff   = i * inFbPkt;
        const int dataOff = i * outDataPkt;

        uint8_t rateVal = (uint8_t)((int32_t)feedbackBuf[fbOff + 0] + 128);
        int     lip     = lipArr[i];

        dataOutBuf[dataOff + 0] = (int8_t)((int32_t)rateVal            - 128);
        dataOutBuf[dataOff + 1] = (int8_t)((int32_t)(lip & 0xFF)       - 128);
        dataOutBuf[dataOff + 2] = (int8_t)((int32_t)((lip >> 8) & 0xFF)- 128);

        if (isFirstBatch && i < DEBUG_PKT_LIMIT) {
            printf("[InterleaverMiddleman] DBG pkt[%d] rate=%u lip=%d -> header set\n",
                   i, (unsigned)rateVal, lip);
        }
    }

    outData.write(dataOutBuf, actualCount);

    customData.frameCount += actualCount;

    delete[] dataBuf;
    delete[] rateBuf;
    delete[] feedbackBuf;
    delete[] signalBuf;
    delete[] dataOutBuf;
    delete[] lipArr;
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
        {3030, 3, 3},               // inputPacketSizes  [INT_DATA, rate_enc_len, feedback]
        {6000, 6000, 6000},         // inputBatchSizes
        {3027, 6},                  // outputPacketSizes [rate+lip+DATA_INT(3027), SIGNAL_INT(6)]
        {6000, 6000},               // outputBatchSizes
        false,                      // ltr (middleman)
        true,                       // startWithAll
        "Interleaver middleman: SIGNAL_INT first; rate+lip(enc_len)+DATA_INT second"
    };

    run_manual_block(pipeIns, pipeOuts, config,
                     process_interleaver_middleman,
                     init_interleaver_middleman);
    return 0;
}

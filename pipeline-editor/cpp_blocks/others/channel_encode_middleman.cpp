#include "core/run_generic_block.h"
#include <cstring>

// ============================================================
// Channel Encode Middleman  (test/bypass block for channel encoder layer)
//
// Sits between channel_encode and channel_decode for isolated testing.
// Will be removed once the channel encoder layer is verified.
//
// Inputs:
//   in[0]: Encoded DATA from channel_encode         (3030 bytes/pkt)  <- FIRST
//   in[1]: rate_encoded_length from channel_encode  (3 bytes/pkt)
//             Byte 0: rate_value  (unused here; rate comes from feedback)
//             Byte 1: enc_len_lo  <- lip for encoded domain
//             Byte 2: enc_len_hi
//   in[2]: Feedback from ppdu_decapsulate           (3 bytes/pkt)
//             Byte 0: rate_value
//             Byte 1: mac_len_lo  (unused)
//             Byte 2: mac_len_hi  (unused)
//
// Outputs:
//   out[0]: rate + lip + DATA_ENCODED -> channel_decode  (3027 bytes/pkt)  <- FIRST
//             Byte 0: rate_value   (from feedback)
//             Byte 1: lip_lo       (= enc_len_lo - 6, DATA portion only)
//             Byte 2: lip_hi       (= enc_len_hi)
//             Bytes [3..3025]: encoded DATA (3030 - 6 = 3024 bytes)
//   out[1]: SIGNAL_ENCODED -> channel_decode             (6 bytes/pkt)
//
// Protocol per batch:
//   1. Read encoded DATA        (in[0])  -- FIRST
//   2. Read rate_encoded_length (in[1])  -- keep enc_len as lip
//   3. Split: first 6 bytes = SIGNAL_ENCODED, rest -> out[0] bytes [3..]
//   4. Send SIGNAL_ENCODED      (out[1]) -- SIGNAL first
//   5. Wait for feedback        (in[2])  -- gate; extract rate_value
//   6. Fill header [rate|lip_lo|lip_hi], send out[0]
// ============================================================

static const int DEBUG_PKT_LIMIT = 3;

struct ChannelEncodeMiddlemanData {
    int frameCount;
};

ChannelEncodeMiddlemanData init_channel_encode_middleman(const BlockConfig& config) {
    ChannelEncodeMiddlemanData data;
    data.frameCount = 0;

    printf("[ChannelEncodeMiddleman] Staged I/O mode\n");
    printf("[ChannelEncodeMiddleman] out[0] layout: [rate(1)|lip_lo(1)|lip_hi(1)|DATA_ENC(3024)]\n");
    printf("[ChannelEncodeMiddleman] lip = enc_len - 6 (DATA portion, excludes SIGNAL 6B)\n");
    printf("[ChannelEncodeMiddleman] Sequence per batch:\n");
    printf("[ChannelEncodeMiddleman]   1. Read encoded DATA (in[0], 3030B)   -- FIRST\n");
    printf("[ChannelEncodeMiddleman]   2. Read rate_enc_len (in[1], 3B)      -- use enc_len as lip\n");
    printf("[ChannelEncodeMiddleman]   3. Send SIGNAL_ENC   (out[1], 6B)     -- SIGNAL first\n");
    printf("[ChannelEncodeMiddleman]   4. Wait for feedback (in[2], 3B)      -- gate for rate\n");
    printf("[ChannelEncodeMiddleman]   5. Send rate+lip+DATA_ENC (out[0], 3027B)\n");
    printf("[ChannelEncodeMiddleman] Debug: first %d packets of first batch logged\n",
           DEBUG_PKT_LIMIT);
    printf("[ChannelEncodeMiddleman] Ready\n");

    return data;
}

void process_channel_encode_middleman(
    const char** pipeIn,
    const char** pipeOut,
    ChannelEncodeMiddlemanData& customData,
    const BlockConfig& config
) {
    PipeIO inEncData (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);  // 3030
    PipeIO inRateEnc (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);  // 3
    PipeIO inFeedback(pipeIn[2],  config.inputPacketSizes[2],  config.inputBatchSizes[2]);  // 3
    PipeIO outData   (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]); // 3026
    PipeIO outSignal (pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]); // 6

    int8_t* encDataBuf  = new int8_t[inEncData.getBufferSize()];
    int8_t* rateEncBuf  = new int8_t[inRateEnc.getBufferSize()];
    int8_t* feedbackBuf = new int8_t[inFeedback.getBufferSize()];
    int8_t* signalBuf   = new int8_t[outSignal.getBufferSize()];
    int8_t* dataOutBuf  = new int8_t[outData.getBufferSize()];

    const int inEncPkt   = config.inputPacketSizes[0];   // 3030
    const int inRatePkt  = config.inputPacketSizes[1];   // 3
    const int inFbPkt    = config.inputPacketSizes[2];   // 3
    const int outDataPkt = config.outputPacketSizes[0];  // 3027
    const int outSigPkt  = config.outputPacketSizes[1];  // 6

    const bool isFirstBatch = (customData.frameCount == 0);

    // ===== STEP 1: Read encoded DATA batch =====
    int actualCount = inEncData.read(encDataBuf);

    // ===== STEP 2: Read rate_encoded_length -- grab enc_len as lip =====
    inRateEnc.read(rateEncBuf);

    memset(signalBuf,  0, outSignal.getBufferSize());
    memset(dataOutBuf, 0, outData.getBufferSize());

    // Store lip per packet before feedback arrives
    int* lipArr = new int[actualCount];

    // ===== STEP 3: Split SIGNAL_ENCODED (6B) and DATA_ENCODED; store lip =====
    for (int i = 0; i < actualCount; i++) {
        const bool dbg    = isFirstBatch && (i < DEBUG_PKT_LIMIT);
        const int encOff  = i * inEncPkt;
        const int rateOff = i * inRatePkt;
        const int sigOff  = i * outSigPkt;
        const int dataOff = i * outDataPkt;

        // Grab enc_len from rate pipe (bytes 1+2); subtract 6 for SIGNAL
        uint8_t encLo = (uint8_t)((int32_t)rateEncBuf[rateOff + 1] + 128);
        uint8_t encHi = (uint8_t)((int32_t)rateEncBuf[rateOff + 2] + 128);
        int encLen = (int)encLo | ((int)encHi << 8);
        int dataLip = encLen - 6;
        if (dataLip < 0) dataLip = 0;
        lipArr[i] = dataLip;

        // First 6 bytes = SIGNAL_ENCODED
        memcpy(signalBuf + sigOff, encDataBuf + encOff, 6);

        // Remaining bytes = DATA_ENCODED -> out[0] bytes [3..] (header TBD)
        int copyLen = inEncPkt - 6;  // 3024
        if (copyLen > outDataPkt - 3) copyLen = outDataPkt - 3;
        memcpy(dataOutBuf + dataOff + 3, encDataBuf + encOff + 6, copyLen);

        if (dbg) {
            uint8_t s0 = (uint8_t)((int32_t)signalBuf[sigOff + 0] + 128);
            uint8_t s1 = (uint8_t)((int32_t)signalBuf[sigOff + 1] + 128);
            uint8_t s2 = (uint8_t)((int32_t)signalBuf[sigOff + 2] + 128);
            uint8_t s3 = (uint8_t)((int32_t)signalBuf[sigOff + 3] + 128);
            uint8_t s4 = (uint8_t)((int32_t)signalBuf[sigOff + 4] + 128);
            uint8_t s5 = (uint8_t)((int32_t)signalBuf[sigOff + 5] + 128);
            printf("[ChannelEncodeMiddleman] DBG pkt[%d] SIGNAL_ENC: "
                   "%02X %02X %02X %02X %02X %02X\n",
                   i, s0, s1, s2, s3, s4, s5);
            printf("[ChannelEncodeMiddleman] DBG pkt[%d] enc_len=%d dataLip=%d\n",
                   i, encLen, dataLip);
        }
    }

    // ===== STEP 4: Send SIGNAL_ENCODED batch FIRST =====
    outSignal.write(signalBuf, actualCount);

    // ===== STEP 5: Wait for feedback (gate) -- extract rate_value =====
    inFeedback.read(feedbackBuf);

    // ===== STEP 6: Fill header [rate | lip_lo | lip_hi], send out[0] =====
    for (int i = 0; i < actualCount; i++) {
        const bool dbg    = isFirstBatch && (i < DEBUG_PKT_LIMIT);
        const int fbOff   = i * inFbPkt;
        const int dataOff = i * outDataPkt;

        uint8_t rateVal = (uint8_t)((int32_t)feedbackBuf[fbOff + 0] + 128);
        int     lip     = lipArr[i];

        dataOutBuf[dataOff + 0] = (int8_t)((int32_t)rateVal             - 128);
        dataOutBuf[dataOff + 1] = (int8_t)((int32_t)(lip & 0xFF)        - 128);
        dataOutBuf[dataOff + 2] = (int8_t)((int32_t)((lip >> 8) & 0xFF) - 128);

        if (dbg) {
            printf("[ChannelEncodeMiddleman] DBG pkt[%d] rate=%u lip=%d -> header set\n",
                   i, (unsigned)rateVal, lip);
        }
    }

    outData.write(dataOutBuf, actualCount);

    customData.frameCount += actualCount;

    delete[] encDataBuf;
    delete[] rateEncBuf;
    delete[] feedbackBuf;
    delete[] signalBuf;
    delete[] dataOutBuf;
    delete[] lipArr;
}

int main(int argc, char* argv[]) {
    if (argc < 6) {
        fprintf(stderr,
            "Usage: channel_encode_middleman <pipeInEncData> <pipeInRateEnc> <pipeInFeedback>"
            " <pipeOutData> <pipeOutSignal>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1], argv[2], argv[3]};
    const char* pipeOuts[] = {argv[4], argv[5]};

    BlockConfig config = {
        "ChannelEncodeMiddleman",
        3,                          // inputs
        2,                          // outputs
        {3030, 3, 3},               // inputPacketSizes  [ENC_DATA, rate_enc_len, feedback]
        {64, 64, 64},         // inputBatchSizes
        {3027, 6},                  // outputPacketSizes [rate+lip+DATA_ENC(3027), SIGNAL_ENC(6)]
        {64, 64},               // outputBatchSizes
        false,                      // ltr (middleman)
        true,                       // startWithAll
        "Channel encoder middleman: SIGNAL_ENC first; rate+lip(enc_len-6)+DATA_ENC second"
    };

    run_manual_block(pipeIns, pipeOuts, config,
                     process_channel_encode_middleman,
                     init_channel_encode_middleman);
    return 0;
}
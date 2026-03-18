#include "core/run_generic_block.h"
#include <cstring>

// ============================================================
// IEEE 802.11a Scrambler
//
// Inputs:
//   in[0]: DATA pipe from ppdu_encapsulate  (1515 bytes/pkt, max)  <- FIRST
//   in[1]: RATE/LIP pipe                    (3 bytes/pkt)
//             Byte 0: rate_value
//             Byte 1: lip_low   (length_in_pipe low byte)
//             Byte 2: lip_high  (length_in_pipe high byte)
//
// Outputs:
//   out[0]: Scrambled DATA pipe             (1515 bytes/pkt, max)  <- FIRST
//   out[1]: RATE/LIP pipe passthrough       (3 bytes/pkt, unchanged)
//
// Processing per packet:
//   - Reads length_in_pipe (lip) from RATE pipe to know how many
//     bytes in the DATA packet are meaningful.
//   - Bytes [0..2]   = SIGNAL field → passed through UNCHANGED
//   - Bytes [3..lip-1] = SERVICE + PSDU + TAIL_PAD → scrambled
//   - Bytes [lip..1514] = trailing zeros → left as zero (don't scramble)
//   - Seed is stored back into SERVICE byte 0 bits [6:0]
//     (SERVICE byte 0 bit 7 is preserved from input, which is 0).
//
// Scrambler polynomial: S(x) = x^7 + x^4 + 1
// Pre-computed LUT for all 127 non-zero seeds.
// ============================================================

struct ScramblerData {
    int     frameCount;
    uint8_t lut[128][1515];  // lut[seed][byte_index] for seeds 1..127
};

ScramblerData init_scrambler(const BlockConfig& config) {
    ScramblerData data;
    data.frameCount = 0;

    printf("[Scrambler] Polynomial: S(x) = x^7 + x^4 + 1\n");
    printf("[Scrambler] Building LUT for seeds 1..127 (1515 bytes each)...\n");

    for (int seed = 1; seed <= 127; seed++) {
        uint8_t state[7];
        for (int b = 0; b < 7; b++) state[b] = (seed >> (6 - b)) & 1;

        for (int byteIdx = 0; byteIdx < 1515; byteIdx++) {
            uint8_t sb = 0;
            for (int bitIdx = 0; bitIdx < 8; bitIdx++) {
                sb |= (state[6] << bitIdx);
                uint8_t nb = state[6] ^ state[3];
                for (int s = 6; s > 0; s--) state[s] = state[s - 1];
                state[0] = nb;
            }
            data.lut[seed][byteIdx] = sb;
        }
    }

    printf("[Scrambler] LUT built (128 × 1515 bytes)\n");
    printf("[Scrambler] DATA pipe: 1515 bytes/pkt (max). lip from RATE pipe bounds processing.\n");
    printf("[Scrambler] Ready\n");

    return data;
}

void process_scrambler(
    const char** pipeIn,
    const char** pipeOut,
    ScramblerData& customData,
    const BlockConfig& config
) {
    // in[0]  = DATA (1515) -- FIRST
    // in[1]  = RATE/LIP (3)
    // out[0] = scrambled DATA (1515) -- FIRST
    // out[1] = RATE/LIP passthrough (3)
    PipeIO inData  (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);
    PipeIO inRate  (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);
    PipeIO outData (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);
    PipeIO outRate (pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]);

    int8_t* dataBuf    = new int8_t[inData.getBufferSize()];
    int8_t* rateBuf    = new int8_t[inRate.getBufferSize()];
    int8_t* dataOutBuf = new int8_t[outData.getBufferSize()];
    int8_t* rateOutBuf = new int8_t[outRate.getBufferSize()];

    const int inDataPkt  = config.inputPacketSizes[0];  // 1515
    const int inRatePkt  = config.inputPacketSizes[1];  // 3
    const int outDataPkt = config.outputPacketSizes[0]; // 1515
    const int outRatePkt = config.outputPacketSizes[1]; // 3

    // STEP 1: Read DATA batch first (rate measurement)
    int actualCount = inData.read(dataBuf);

    // STEP 2: Read RATE/LIP batch
    inRate.read(rateBuf);

    // Passthrough RATE/LIP unchanged
    memcpy(rateOutBuf, rateBuf, outRate.getBufferSize());

    // Zero-initialise output DATA buffer
    memset(dataOutBuf, 0, outData.getBufferSize());

    // STEP 3: Scramble each packet
    for (int i = 0; i < actualCount; i++) {
        const int dataOff = i * inDataPkt;
        const int rateOff = i * inRatePkt;
        const int outOff  = i * outDataPkt;

        // Extract lip from RATE pipe
        uint8_t  lipLo = (uint8_t)((int32_t)rateBuf[rateOff + 1] + 128);
        uint8_t  lipHi = (uint8_t)((int32_t)rateBuf[rateOff + 2] + 128);
        int lip = (int)lipLo | ((int)lipHi << 8);
        if (lip > inDataPkt) lip = inDataPkt;

        // Convert meaningful bytes to uint8
        uint8_t pkt[1515];
        for (int j = 0; j < lip; j++) {
            pkt[j] = (uint8_t)((int32_t)dataBuf[dataOff + j] + 128);
        }

        // SIGNAL (bytes 0..2): copy unchanged
        uint8_t out[1515];
        memset(out, 0, sizeof(out));
        out[0] = pkt[0];
        out[1] = pkt[1];
        out[2] = pkt[2];

        // Choose seed: deterministic from frame counter
        int seed = ((customData.frameCount + i) * 37 + 127) % 127 + 1; // 1..127
        const uint8_t* seq = customData.lut[seed];

        // Scramble bytes [3..lip-1]
        for (int j = 3; j < lip; j++) {
            out[j] = pkt[j] ^ seq[j];
        }

        // Store seed in SERVICE byte 0 bits [6:0] (bit 7 preserved = 0)
        out[3] = (out[3] & 0x80) | (uint8_t)(seed & 0x7F);

        // Convert back to int8 (only meaningful bytes; rest remain 0)
        for (int j = 0; j < lip; j++) {
            dataOutBuf[outOff + j] = (int8_t)((int32_t)out[j] - 128);
        }
    }

    // STEP 4: Send DATA first (rate measurement), then RATE/LIP
    outData.write(dataOutBuf, actualCount);
    outRate.write(rateOutBuf, actualCount);

    customData.frameCount += actualCount;

    delete[] dataBuf;
    delete[] rateBuf;
    delete[] dataOutBuf;
    delete[] rateOutBuf;
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: scrambler <pipeInData> <pipeInRate> <pipeOutData> <pipeOutRate>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1], argv[2]};
    const char* pipeOuts[] = {argv[3], argv[4]};

    BlockConfig config = {
        "Scrambler",
        2,                       // inputs
        2,                       // outputs
        {1515, 3},               // inputPacketSizes  [DATA_max, RATE/LIP]
        {64, 64},          // inputBatchSizes
        {1515, 3},               // outputPacketSizes [DATA_max, RATE/LIP]
        {64, 64},          // outputBatchSizes
        true,                    // ltr
        true,                    // startWithAll
        "Scrambler: SIGNAL passthrough, DATA scrambled up to lip bytes, RATE/LIP passthrough"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_scrambler, init_scrambler);
    return 0;
}

#include "core/run_generic_block.h"
#include <cstring>

struct ScramblerData {
    int frameCount;
    uint8_t lut[128][1515];
};

ScramblerData init_scrambler(const BlockConfig& config) {
    ScramblerData data;
    data.frameCount = 0;



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


    return data;
}

void process_scrambler(
    const char** pipeIn,
    const char** pipeOut,
    ScramblerData& customData,
    const BlockConfig& config
) {
    PipeIO inData (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);
    PipeIO inRate (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);
    PipeIO outData(pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);
    PipeIO outRate(pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]);

    int8_t* dataBuf    = new int8_t[inData.getBufferSize()];
    int8_t* rateBuf    = new int8_t[inRate.getBufferSize()];
    int8_t* dataOutBuf = new int8_t[outData.getBufferSize()];
    int8_t* rateOutBuf = new int8_t[outRate.getBufferSize()];

    const int inDataPkt  = config.inputPacketSizes[0];   // 1515
    const int inRatePkt  = config.inputPacketSizes[1];   // 5
    const int outDataPkt = config.outputPacketSizes[0];  // 1515

    int actualCount = inData.read(dataBuf);
    inRate.read(rateBuf);

    // Rate pipe is passed through unchanged
    memcpy(rateOutBuf, rateBuf, outRate.getBufferSize());
    memset(dataOutBuf, 0, outData.getBufferSize());

    const bool isFirstBatch = (customData.frameCount == 0);



    for (int i = 0; i < actualCount; i++) {
        const bool dbg = isFirstBatch && (i == 0);

        const int dataOff = i * inDataPkt;
        const int rateOff = i * inRatePkt;
        const int outOff  = i * outDataPkt;

        // ------------------------------------------------------------------
        // Decode rate pipe
        // ------------------------------------------------------------------
        uint8_t rateVal = (uint8_t)((int32_t)rateBuf[rateOff + 0] + 128);
        uint8_t b1      = (uint8_t)((int32_t)rateBuf[rateOff + 1] + 128);
        uint8_t b2      = (uint8_t)((int32_t)rateBuf[rateOff + 2] + 128);
        uint8_t b3      = (uint8_t)((int32_t)rateBuf[rateOff + 3] + 128);
        uint8_t b4      = (uint8_t)((int32_t)rateBuf[rateOff + 4] + 128);
        uint32_t lipBits = (uint32_t)b1
                         | ((uint32_t)b2 <<  8)
                         | ((uint32_t)b3 << 16)
                         | ((uint32_t)b4 << 24);

        int lipBitCount  = (int)lipBits;
        int lipByteCount = (lipBitCount + 7) / 8;
        if (lipByteCount > inDataPkt) lipByteCount = inDataPkt;

        // ------------------------------------------------------------------
        // Read input packet into unsigned buffer
        // ------------------------------------------------------------------
        uint8_t pkt[1515];
        for (int j = 0; j < lipByteCount; j++)
            pkt[j] = (uint8_t)((int32_t)dataBuf[dataOff + j] + 128);

        uint8_t out[1515];
        memset(out, 0, sizeof(out));

        // ------------------------------------------------------------------
        // SIGNAL field (bits 0–23, 3 bytes) — always passthrough
        // ------------------------------------------------------------------
        const int SIGNAL_BITS  = 24;
        const int signalBytes  = SIGNAL_BITS / 8;   // = 3

        for (int j = 0; j < signalBytes; j++)
            out[j] = pkt[j];

        // ------------------------------------------------------------------
        // DATA field (bits 24 .. lipBits-1) — scramble with LFSR
        // ------------------------------------------------------------------
        const int dataStartBit = SIGNAL_BITS;
        const int dataBitCount = lipBitCount - SIGNAL_BITS;

        int seed = ((customData.frameCount + i) * 37 + 127) % 127 + 1;

        if (dataBitCount > 0) {
            const uint8_t* seq = customData.lut[seed];

            uint8_t* inBits  = new uint8_t[dataBitCount];
            uint8_t* outBits = new uint8_t[dataBitCount];

            // Extract input bits starting from bit 24
            for (int bit = 0; bit < dataBitCount; bit++) {
                int byteIdx = (dataStartBit + bit) / 8;
                int bitIdx  = (dataStartBit + bit) % 8;
                inBits[bit] = (pkt[byteIdx] >> bitIdx) & 1;
            }

            // XOR with scrambler sequence bits (same offset in sequence)
            for (int bit = 0; bit < dataBitCount; bit++) {
                int seqByteIdx = (dataStartBit + bit) / 8;
                int seqBitIdx  = (dataStartBit + bit) % 8;
                uint8_t seqBit = (seq[seqByteIdx] >> seqBitIdx) & 1;
                outBits[bit]   = inBits[bit] ^ seqBit;
            }

            // Repack scrambled bits back into output bytes
            // (bytes 0..signalBytes-1 already hold SIGNAL passthrough)
            for (int byteIdx = signalBytes; byteIdx < lipByteCount; byteIdx++) {
                out[byteIdx] = 0;
                for (int bit = 0; bit < 8; bit++) {
                    int globalBit = byteIdx * 8 + bit;
                    if (globalBit >= dataStartBit && globalBit < dataStartBit + dataBitCount) {
                        int outBitIdx = globalBit - dataStartBit;
                        out[byteIdx] |= (outBits[outBitIdx] << bit);
                    }
                }
            }

            delete[] inBits;
            delete[] outBits;
        } else {
            // Edge case: no data bits (should not happen with 1504-byte PSDU)
            for (int j = signalBytes; j < lipByteCount; j++)
                out[j] = pkt[j];
        }

        // ------------------------------------------------------------------
        // Embed scrambler seed into SERVICE field, byte 3 (bits 24–30)
        // Bit 31 (MSB of byte 3) is reserved and preserved from scrambled output.
        // ------------------------------------------------------------------
        if (lipByteCount > 3 && lipBitCount > 24) {
            out[3] = (out[3] & 0x80) | (uint8_t)(seed & 0x7F);
        }

        // Convert back to int8 pipe representation
        for (int j = 0; j < lipByteCount; j++)
            dataOutBuf[outOff + j] = (int8_t)((int32_t)out[j] - 128);

        if (dbg) {
            printf("[Scrambler] pkt[0] INPUT : signal=%d  data=%d  total=%d bits\n",
                   SIGNAL_BITS, dataBitCount, lipBitCount);
            printf("[Scrambler] pkt[0] OUTPUT: signal=%d  data=%d  total=%d bits\n",
                   SIGNAL_BITS, dataBitCount, lipBitCount);
            fflush(stdout);
        }
    }



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
        2,             // inputs
        2,             // outputs
        {1515, 5},     // inputPacketSizes  [DATA_buf, RATE/lipBits_LE]
        {64, 64},      // inputBatchSizes
        {1515, 5},     // outputPacketSizes [DATA_buf, RATE/lipBits_LE]
        {64, 64},      // outputBatchSizes
        true,          // ltr
        true,          // startWithAll
        "Scrambler: bit-level, SIGNAL passthrough, DATA scrambled, exact bit counts"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_scrambler, init_scrambler);
    return 0;
}
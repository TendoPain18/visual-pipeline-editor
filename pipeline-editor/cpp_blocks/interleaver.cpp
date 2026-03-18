#include "core/run_generic_block.h"
#include <cstring>
#include <cstdint>

// ============================================================
// IEEE 802.11a Interleaver
//
// Inputs:
//   in[0]: Encoded DATA pipe from channel_encode  (3030 bytes/pkt, max)
//   in[1]: rate_encoded_length pipe               (5 bytes/pkt)
//             Byte 0: rate_value
//             Bytes 1-4: encoded_len_bits (uint32 LE) -- EXACT encoded bit count
//
// Outputs:
//   out[0]: Interleaved DATA pipe                 (3030 bytes/pkt, max)
//   out[1]: rate_encoded_length passthrough       (5 bytes/pkt, unchanged)
//
// Processing per packet:
//   - First 48 bits  = SIGNAL field -> interleaved with BPSK params (NCBPS=48)
//   - Remaining bits = DATA field   -> interleaved with rate-dependent params
//
// Interleaver permutation (IEEE 802.11a, two-step):
//   Step 1: k -> i = (NCBPS/16)*mod(k,16) + floor(k/16)
//   Step 2: i -> j = s*floor(i/s) + mod(i + NCBPS - floor(16*i/NCBPS), s)
//   where s = max(floor(NBPSC/2), 1)
// ============================================================

struct RateModParams { int NBPSC; int NCBPS; };

static RateModParams getModParams(uint8_t rateVal) {
    switch (rateVal) {
        case 13: return {1,  48};
        case 15: return {1,  48};
        case  5: return {2,  96};
        case  7: return {2,  96};
        case  9: return {4, 192};
        case 11: return {4, 192};
        case  1: return {6, 288};
        case  3: return {6, 288};
        default: return {2,  96};
    }
}

static const char* rateNameFromVal(uint8_t rateVal) {
    switch (rateVal) {
        case 13: return "6 Mbps (BPSK 1/2)";
        case 15: return "9 Mbps (BPSK 3/4)";
        case  5: return "12 Mbps (QPSK 1/2)";
        case  7: return "18 Mbps (QPSK 3/4)";
        case  9: return "24 Mbps (16-QAM 1/2)";
        case 11: return "36 Mbps (16-QAM 3/4)";
        case  1: return "48 Mbps (64-QAM 2/3)";
        case  3: return "54 Mbps (64-QAM 3/4)";
        default: return "UNKNOWN";
    }
}

static void interleave_block(const uint8_t* bits, uint8_t* out, int NCBPS, int NBPSC) {
    int s = (NBPSC / 2 > 1) ? NBPSC / 2 : 1;
    for (int k = 0; k < NCBPS; k++) {
        int i = (NCBPS / 16) * (k % 16) + k / 16;
        int j = s * (i / s) + (i + NCBPS - (16 * i / NCBPS)) % s;
        out[j] = bits[k];
    }
}

struct InterleaverData { int frameCount; };

InterleaverData init_interleaver(const BlockConfig& config) {
    InterleaverData data;
    data.frameCount = 0;

    return data;
}

void process_interleaver(
    const char** pipeIn,
    const char** pipeOut,
    InterleaverData& customData,
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

    const int inDataPkt  = config.inputPacketSizes[0];   // 3030
    const int inRatePkt  = config.inputPacketSizes[1];   // 5
    const int outDataPkt = config.outputPacketSizes[0];  // 3030

    const bool isFirstBatch = (customData.frameCount == 0);

    int actualCount = inData.read(dataBuf);
    inRate.read(rateBuf);

    // Rate pipe passthrough unchanged
    memcpy(rateOutBuf, rateBuf, outRate.getBufferSize());
    memset(dataOutBuf, 0, outData.getBufferSize());



    for (int i = 0; i < actualCount; i++) {
        const bool dbg = isFirstBatch && (i == 0);

        const int dataOff = i * inDataPkt;
        const int rateOff = i * inRatePkt;
        const int outOff  = i * outDataPkt;

        // Decode rate pipe
        uint8_t rateVal = (uint8_t)((int32_t)rateBuf[rateOff + 0] + 128);
        uint8_t b1      = (uint8_t)((int32_t)rateBuf[rateOff + 1] + 128);
        uint8_t b2      = (uint8_t)((int32_t)rateBuf[rateOff + 2] + 128);
        uint8_t b3      = (uint8_t)((int32_t)rateBuf[rateOff + 3] + 128);
        uint8_t b4      = (uint8_t)((int32_t)rateBuf[rateOff + 4] + 128);
        uint32_t encLenBits = (uint32_t)b1 | ((uint32_t)b2 << 8)
                            | ((uint32_t)b3 << 16) | ((uint32_t)b4 << 24);

        int encBitCount  = (int)encLenBits;  // EXACT bits
        int encByteCount = (encBitCount + 7) / 8;
        if (encByteCount > inDataPkt) encByteCount = inDataPkt;

        // Convert input bytes -> bit array
        uint8_t* inBits  = new uint8_t[encBitCount]();
        uint8_t* outBits = new uint8_t[encBitCount]();

        for (int j = 0; j < encByteCount; j++) {
            uint8_t b = (uint8_t)((int32_t)dataBuf[dataOff + j] + 128);
            for (int bit = 0; bit < 8; bit++) {
                int idx = j * 8 + bit;
                if (idx < encBitCount)
                    inBits[idx] = (b >> bit) & 1;
            }
        }

        // ------------------------------------------------------------------
        // SIGNAL field: bits 0–47 (48 bits), always BPSK (NCBPS=48, NBPSC=1)
        // ------------------------------------------------------------------
        const int SIGNAL_ENC_BITS = 48;

        if (encBitCount >= SIGNAL_ENC_BITS) {
            uint8_t sigIn[48], sigOut[48];
            for (int b = 0; b < SIGNAL_ENC_BITS; b++) sigIn[b] = inBits[b];
            interleave_block(sigIn, sigOut, 48, 1);
            for (int b = 0; b < SIGNAL_ENC_BITS; b++) outBits[b] = sigOut[b];
        } else {
            for (int b = 0; b < encBitCount; b++) outBits[b] = inBits[b];
        }

        // ------------------------------------------------------------------
        // DATA field: bits 48..encBits-1, rate-dependent NCBPS
        // ------------------------------------------------------------------
        RateModParams mp = getModParams(rateVal);
        int NCBPS = mp.NCBPS, NBPSC = mp.NBPSC;

        int dataBitCount = encBitCount - SIGNAL_ENC_BITS;
        int numCompleteSymbols = (dataBitCount > 0) ? dataBitCount / NCBPS : 0;

        if (dataBitCount > 0) {
            uint8_t* symIn  = new uint8_t[NCBPS];
            uint8_t* symOut = new uint8_t[NCBPS];

            for (int sym = 0; sym < numCompleteSymbols; sym++) {
                int start = SIGNAL_ENC_BITS + sym * NCBPS;
                for (int b = 0; b < NCBPS; b++) symIn[b] = inBits[start + b];
                interleave_block(symIn, symOut, NCBPS, NBPSC);
                for (int b = 0; b < NCBPS; b++) outBits[start + b] = symOut[b];
            }

            // Partial last symbol (should not occur with fixed lipBits, but handle safely)
            int processed = SIGNAL_ENC_BITS + numCompleteSymbols * NCBPS;
            if (processed < encBitCount) {
                int partialBits = encBitCount - processed;
                for (int b = 0; b < partialBits; b++)  symIn[b] = inBits[processed + b];
                for (int b = partialBits; b < NCBPS; b++) symIn[b] = 0;
                interleave_block(symIn, symOut, NCBPS, NBPSC);
                for (int b = 0; b < partialBits; b++) outBits[processed + b] = symOut[b];
            }

            delete[] symIn;
            delete[] symOut;
        }

        // Pack bits -> output bytes
        for (int byte = 0; byte < encByteCount; byte++) {
            uint8_t bval = 0;
            for (int bit = 0; bit < 8; bit++) {
                int idx = byte * 8 + bit;
                if (idx < encBitCount) bval |= (outBits[idx] & 1) << bit;
            }
            dataOutBuf[outOff + byte] = (int8_t)((int32_t)bval - 128);
        }

        if (dbg) {
            printf("[Interleaver] pkt[0] INPUT : signal=%d  data=%d  total=%d bits\n",
                   SIGNAL_ENC_BITS, dataBitCount, encBitCount);
            printf("[Interleaver] pkt[0] OUTPUT: signal=%d  data=%d  total=%d bits\n",
                   SIGNAL_ENC_BITS, dataBitCount, encBitCount);
            fflush(stdout);
        }

        delete[] inBits;
        delete[] outBits;
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
        fprintf(stderr,
            "Usage: interleaver <pipeInData> <pipeInRate> <pipeOutData> <pipeOutRate>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1], argv[2]};
    const char* pipeOuts[] = {argv[3], argv[4]};

    BlockConfig config = {
        "Interleaver",
        2,             // inputs
        2,             // outputs
        {3030, 5},     // inputPacketSizes  [ENC_DATA, rate_enc_len_bits(uint32 LE)]
        {64, 64},      // inputBatchSizes
        {3030, 5},     // outputPacketSizes [INTERLEAVED_DATA, rate_enc_len_bits(uint32 LE)]
        {64, 64},      // outputBatchSizes
        true,          // ltr
        true,          // startWithAll
        "IEEE 802.11a two-step interleaver: SIGNAL@BPSK, DATA@rate-params, exact bit counts"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_interleaver, init_interleaver);
    return 0;
}
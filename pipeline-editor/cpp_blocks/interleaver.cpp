#include "core/run_generic_block.h"
#include <cstring>
#include <cstdint>

// ============================================================
// IEEE 802.11a Interleaver
//
// Inputs:
//   in[0]: Encoded DATA pipe from channel_encode  (3030 bytes/pkt, max)  <- FIRST
//   in[1]: rate_encoded_length pipe               (3 bytes/pkt)
//             Byte 0: rate_value
//             Byte 1: encoded_len_lo
//             Byte 2: encoded_len_hi
//
// Outputs:
//   out[0]: Interleaved DATA pipe                 (3030 bytes/pkt, max)  <- FIRST
//   out[1]: rate_encoded_length passthrough       (3 bytes/pkt, unchanged)
//
// Processing per packet:
//   - Reads encoded_length from RATE pipe to know how many bytes are meaningful.
//   - First 6 bytes (48 bits) = SIGNAL field -> interleaved with BPSK params (NCBPS=48)
//   - Remaining bytes = DATA field -> interleaved with rate-dependent params
//   - Bytes beyond encoded_length are zero-padded in output
//
// Interleaver permutation (IEEE 802.11a, two-step):
//   Step 1: k -> i = (NCBPS/16)*mod(k,16) + floor(k/16)
//   Step 2: i -> j = s*floor(i/s) + mod(i + NCBPS - floor(16*i/NCBPS), s)
//   where s = max(floor(NBPSC/2), 1)
//
// Rate -> (NBPSC, NCBPS):
//   13 ->  6 Mbps: BPSK,   NBPSC=1, NCBPS=48
//   15 ->  9 Mbps: BPSK,   NBPSC=1, NCBPS=48
//    5 -> 12 Mbps: QPSK,   NBPSC=2, NCBPS=96
//    7 -> 18 Mbps: QPSK,   NBPSC=2, NCBPS=96
//    9 -> 24 Mbps: 16-QAM, NBPSC=4, NCBPS=192
//   11 -> 36 Mbps: 16-QAM, NBPSC=4, NCBPS=192
//    1 -> 48 Mbps: 64-QAM, NBPSC=6, NCBPS=288
//    3 -> 54 Mbps: 64-QAM, NBPSC=6, NCBPS=288
// ============================================================

struct RateModParams {
    int NBPSC;
    int NCBPS;
};

static RateModParams getModParams(uint8_t rateVal) {
    switch (rateVal) {
        case 13: return {1,  48};   //  6 Mbps BPSK
        case 15: return {1,  48};   //  9 Mbps BPSK
        case  5: return {2,  96};   // 12 Mbps QPSK
        case  7: return {2,  96};   // 18 Mbps QPSK
        case  9: return {4, 192};   // 24 Mbps 16-QAM
        case 11: return {4, 192};   // 36 Mbps 16-QAM
        case  1: return {6, 288};   // 48 Mbps 64-QAM
        case  3: return {6, 288};   // 54 Mbps 64-QAM
        default: return {2,  96};   // fallback: 12 Mbps QPSK
    }
}

// Interleave a block of bits (in-place, permutes bit[k] -> position j).
// 'bits' has 'NCBPS' entries. 'out' receives the permuted bits.
static void interleave_block(const uint8_t* bits, uint8_t* out, int NCBPS, int NBPSC) {
    int s = (NBPSC / 2 > 1) ? NBPSC / 2 : 1;
    for (int k = 0; k < NCBPS; k++) {
        int i = (NCBPS / 16) * (k % 16) + k / 16;
        int j = s * (i / s) + (i + NCBPS - (16 * i / NCBPS)) % s;
        out[j] = bits[k];
    }
}

struct InterleaverData {
    int frameCount;
};

InterleaverData init_interleaver(const BlockConfig& config) {
    InterleaverData data;
    data.frameCount = 0;

    printf("[Interleaver] IEEE 802.11a two-step interleaver\n");
    printf("[Interleaver] SIGNAL field: BPSK, NCBPS=48 (always)\n");
    printf("[Interleaver] DATA field:   rate-dependent NCBPS\n");
    printf("[Interleaver] DATA pipe: 3030 bytes/pkt, RATE pipe: 3 bytes/pkt\n");
    printf("[Interleaver] Ready\n");

    return data;
}

void process_interleaver(
    const char** pipeIn,
    const char** pipeOut,
    InterleaverData& customData,
    const BlockConfig& config
) {
    // in[0]  = Encoded DATA (3030) -- FIRST (rate measurement)
    // in[1]  = rate_encoded_length (3)
    // out[0] = Interleaved DATA (3030) -- FIRST
    // out[1] = rate_encoded_length passthrough (3)
    PipeIO inData  (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);
    PipeIO inRate  (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);
    PipeIO outData (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);
    PipeIO outRate (pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]);

    int8_t* dataBuf    = new int8_t[inData.getBufferSize()];
    int8_t* rateBuf    = new int8_t[inRate.getBufferSize()];
    int8_t* dataOutBuf = new int8_t[outData.getBufferSize()];
    int8_t* rateOutBuf = new int8_t[outRate.getBufferSize()];

    const int inDataPkt  = config.inputPacketSizes[0];   // 3030
    const int inRatePkt  = config.inputPacketSizes[1];   // 3
    const int outDataPkt = config.outputPacketSizes[0];  // 3030

    // STEP 1: Read DATA batch first (rate measurement)
    int actualCount = inData.read(dataBuf);

    // STEP 2: Read RATE/encoded_length batch
    inRate.read(rateBuf);

    // Passthrough RATE unchanged
    memcpy(rateOutBuf, rateBuf, outRate.getBufferSize());
    memset(dataOutBuf, 0, outData.getBufferSize());

    // STEP 3: Interleave each packet
    for (int i = 0; i < actualCount; i++) {
        const int dataOff = i * inDataPkt;
        const int rateOff = i * inRatePkt;
        const int outOff  = i * outDataPkt;

        // Extract rate and encoded length from RATE pipe
        uint8_t rateVal    = (uint8_t)((int32_t)rateBuf[rateOff + 0] + 128);
        uint8_t encLenLo   = (uint8_t)((int32_t)rateBuf[rateOff + 1] + 128);
        uint8_t encLenHi   = (uint8_t)((int32_t)rateBuf[rateOff + 2] + 128);
        int encodedLength  = (int)encLenLo | ((int)encLenHi << 8);
        if (encodedLength > inDataPkt) encodedLength = inDataPkt;

        // Convert meaningful bytes to uint8
        uint8_t pkt[3030];
        for (int j = 0; j < encodedLength; j++) {
            pkt[j] = (uint8_t)((int32_t)dataBuf[dataOff + j] + 128);
        }

        // Expand to bits
        int numBits = encodedLength * 8;
        uint8_t* inBits = new uint8_t[numBits]();
        for (int byte = 0; byte < encodedLength; byte++) {
            for (int bit = 0; bit < 8; bit++) {
                inBits[byte * 8 + bit] = (pkt[byte] >> bit) & 1;  // right-MSB order
            }
        }

        uint8_t* outBits = new uint8_t[numBits]();

        // --- SIGNAL field: first 48 bits, always BPSK (NCBPS=48, NBPSC=1) ---
        if (numBits >= 48) {
            interleave_block(inBits, outBits, 48, 1);
        } else {
            memcpy(outBits, inBits, numBits);
        }

        // --- DATA field: bits 48 onward, rate-dependent ---
        if (numBits > 48) {
            RateModParams mp = getModParams(rateVal);
            int NCBPS = mp.NCBPS;
            int NBPSC = mp.NBPSC;
            int dataBitCount = numBits - 48;
            int numSymbols = dataBitCount / NCBPS;

            uint8_t* symIn  = new uint8_t[NCBPS];
            uint8_t* symOut = new uint8_t[NCBPS];

            for (int sym = 0; sym < numSymbols; sym++) {
                int start = 48 + sym * NCBPS;
                memcpy(symIn, inBits + start, NCBPS);
                interleave_block(symIn, symOut, NCBPS, NBPSC);
                memcpy(outBits + start, symOut, NCBPS);
            }

            // Remaining bits (partial symbol): copy unchanged
            int processed = 48 + numSymbols * NCBPS;
            if (processed < numBits) {
                memcpy(outBits + processed, inBits + processed, numBits - processed);
            }

            delete[] symIn;
            delete[] symOut;
        }

        // Pack bits back to bytes
        uint8_t outBytes[3030] = {};
        for (int byte = 0; byte < encodedLength; byte++) {
            uint8_t b = 0;
            for (int bit = 0; bit < 8; bit++) {
                b |= (outBits[byte * 8 + bit] & 1) << bit;
            }
            outBytes[byte] = b;
        }

        // Convert uint8 -> int8
        for (int j = 0; j < encodedLength; j++) {
            dataOutBuf[outOff + j] = (int8_t)((int32_t)outBytes[j] - 128);
        }
        // Bytes [encodedLength..outDataPkt-1] remain 0 from memset

        delete[] inBits;
        delete[] outBits;
    }

    // STEP 4: Send DATA first (rate measurement), then RATE passthrough
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
        2,                        // inputs
        2,                        // outputs
        {3030, 3},                // inputPacketSizes  [ENC_DATA, rate_enc_len]
        {64, 64},             // inputBatchSizes
        {3030, 3},                // outputPacketSizes [INTERLEAVED_DATA, rate_enc_len]
        {64, 64},             // outputBatchSizes
        true,                     // ltr
        true,                     // startWithAll
        "IEEE 802.11a two-step interleaver: SIGNAL=BPSK, DATA=rate-dependent, RATE passthrough"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_interleaver, init_interleaver);
    return 0;
}

#include "core/run_generic_block.h"
#include <cstring>
#include <cstdint>

// ============================================================
// IEEE 802.11a Deinterleaver
//
// Inputs:
//   in[0]: rate + lip + DATA_INTERLEAVED        (3027 bytes/pkt)  <- [0] rate counter
//             Byte 0: rate_value
//             Byte 1: lip_lo   (meaningful encoded DATA bytes, excl. SIGNAL 6B)
//             Byte 2: lip_hi
//             Bytes [3..3026]: interleaved DATA bits (3024 bytes)
//   in[1]: SIGNAL_INTERLEAVED                   (6 bytes/pkt)
//
// Outputs:
//   out[0]: rate + lip + DATA_DEINTERLEAVED     (3027 bytes/pkt)  <- [0] rate counter
//             Byte 0: rate_value  (passthrough)
//             Byte 1: lip_lo     (passthrough)
//             Byte 2: lip_hi     (passthrough)
//             Bytes [3..3026]: deinterleaved DATA bits (3024 bytes)
//   out[1]: Deinterleaved SIGNAL                (6 bytes/pkt)
//
// lip = number of meaningful encoded DATA bytes (after stripping SIGNAL 6B).
// Only lip bytes in [3..3+lip-1] are deinterleaved; the rest remain zero.
//
// Protocol (deadlock-free):
//   1. Read SIGNAL_INTERLEAVED (in[1])  -- arrives FIRST
//   2. Deinterleave SIGNAL (BPSK, NCBPS=48), send (out[1])
//      -> ppdu_decap reads SIGNAL, sends feedback to middleman
//      -> middleman sends DATA to us
//   3. Read DATA (in[0])
//   4. Read lip from header; deinterleave lip bytes with rate-dependent params
//   5. Send rate+lip+DATA_DEINTERLEAVED (out[0])
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

static void deinterleave_block(const uint8_t* bits, uint8_t* out, int NCBPS, int NBPSC) {
    int s = (NBPSC / 2 > 1) ? NBPSC / 2 : 1;
    int* fwd = new int[NCBPS];
    for (int k = 0; k < NCBPS; k++) {
        int i = (NCBPS / 16) * (k % 16) + k / 16;
        int j = s * (i / s) + (i + NCBPS - (16 * i / NCBPS)) % s;
        fwd[k] = j;
    }
    for (int k = 0; k < NCBPS; k++) out[k] = bits[fwd[k]];
    delete[] fwd;
}

struct DeinterleaverData { int frameCount; };

DeinterleaverData init_deinterleaver(const BlockConfig& config) {
    DeinterleaverData data;
    data.frameCount = 0;

    printf("[Deinterleaver] IEEE 802.11a two-step deinterleaver (inverse)\n");
    printf("[Deinterleaver] in[0]: [rate(1)|lip_lo(1)|lip_hi(1)|DATA_INT(3024)] = 3027B\n");
    printf("[Deinterleaver] out[0]: same layout, DATA deinterleaved up to lip bytes\n");
    printf("[Deinterleaver] lip = meaningful encoded DATA bytes (excl. SIGNAL 6B)\n");
    printf("[Deinterleaver] Protocol (deadlock-free):\n");
    printf("[Deinterleaver]   1. Read SIGNAL_INT (in[1]) -- arrives first\n");
    printf("[Deinterleaver]   2. Deinterleave + send SIGNAL (out[1])\n");
    printf("[Deinterleaver]   3. Read DATA (in[0]) -- arrives after middleman gets feedback\n");
    printf("[Deinterleaver]   4. Deinterleave DATA up to lip bytes\n");
    printf("[Deinterleaver]   5. Send rate+lip+DATA_DEINT (out[0])\n");
    printf("[Deinterleaver] Ready\n");

    return data;
}

void process_deinterleaver(
    const char** pipeIn,
    const char** pipeOut,
    DeinterleaverData& customData,
    const BlockConfig& config
) {
    PipeIO inData    (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);  // 3027
    PipeIO inSignal  (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);  // 6
    PipeIO outData   (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]); // 3027
    PipeIO outSignal (pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]); // 6

    int8_t* signalBuf  = new int8_t[inSignal.getBufferSize()];
    int8_t* dataBuf    = new int8_t[inData.getBufferSize()];
    int8_t* sigOutBuf  = new int8_t[outSignal.getBufferSize()];
    int8_t* dataOutBuf = new int8_t[outData.getBufferSize()];

    const int inSigPkt   = config.inputPacketSizes[1];   // 6
    const int inDataPkt  = config.inputPacketSizes[0];   // 3027
    const int outSigPkt  = config.outputPacketSizes[1];  // 6
    const int outDataPkt = config.outputPacketSizes[0];  // 3027

    // ===== STEP 1: Read SIGNAL_INTERLEAVED -- arrives first =====
    int actualCount = inSignal.read(signalBuf);

    memset(sigOutBuf,  0, outSignal.getBufferSize());
    memset(dataOutBuf, 0, outData.getBufferSize());

    // ===== STEP 2: Deinterleave SIGNAL and send immediately =====
    for (int i = 0; i < actualCount; i++) {
        const int sigOff    = i * inSigPkt;
        const int sigOutOff = i * outSigPkt;

        uint8_t sigBytes[6];
        for (int j = 0; j < 6; j++)
            sigBytes[j] = (uint8_t)((int32_t)signalBuf[sigOff + j] + 128);

        uint8_t inBits[48], outBits[48];
        for (int byte = 0; byte < 6; byte++)
            for (int bit = 0; bit < 8; bit++)
                inBits[byte * 8 + bit] = (sigBytes[byte] >> bit) & 1;

        deinterleave_block(inBits, outBits, 48, 1);

        uint8_t outBytes[6] = {};
        for (int byte = 0; byte < 6; byte++)
            for (int bit = 0; bit < 8; bit++)
                outBytes[byte] |= (outBits[byte * 8 + bit] & 1) << bit;

        for (int j = 0; j < 6; j++)
            sigOutBuf[sigOutOff + j] = (int8_t)((int32_t)outBytes[j] - 128);
    }

    // Send SIGNAL -- ppdu_decap parses it, sends feedback, middleman sends DATA
    outSignal.write(sigOutBuf, actualCount);

    // ===== STEP 3: Read DATA -- arrives after middleman gets feedback =====
    inData.read(dataBuf);

    // ===== STEP 4+5: Deinterleave DATA for each packet =====
    for (int i = 0; i < actualCount; i++) {
        const int dataOff    = i * inDataPkt;
        const int dataOutOff = i * outDataPkt;

        // Read header
        uint8_t rateVal = (uint8_t)((int32_t)dataBuf[dataOff + 0] + 128);
        uint8_t lipLo   = (uint8_t)((int32_t)dataBuf[dataOff + 1] + 128);
        uint8_t lipHi   = (uint8_t)((int32_t)dataBuf[dataOff + 2] + 128);
        int lip = (int)lipLo | ((int)lipHi << 8);
        if (lip > 3024) lip = 3024;   // cap at available DATA bytes

        // Passthrough header to output unchanged
        dataOutBuf[dataOutOff + 0] = dataBuf[dataOff + 0];
        dataOutBuf[dataOutOff + 1] = dataBuf[dataOff + 1];
        dataOutBuf[dataOutOff + 2] = dataBuf[dataOff + 2];

        // Convert lip DATA bytes to uint8 (from offset 3 in packet)
        uint8_t dataBytes[3024] = {};
        for (int j = 0; j < lip; j++)
            dataBytes[j] = (uint8_t)((int32_t)dataBuf[dataOff + 3 + j] + 128);

        RateModParams mp = getModParams(rateVal);
        int NCBPS = mp.NCBPS;
        int NBPSC = mp.NBPSC;

        // Expand to bits, deinterleave symbol by symbol
        int numBits = lip * 8;
        uint8_t* inBits  = new uint8_t[numBits]();
        uint8_t* outBits = new uint8_t[numBits]();

        for (int byte = 0; byte < lip; byte++)
            for (int bit = 0; bit < 8; bit++)
                inBits[byte * 8 + bit] = (dataBytes[byte] >> bit) & 1;

        int numSymbols = numBits / NCBPS;
        uint8_t* symIn  = new uint8_t[NCBPS];
        uint8_t* symOut = new uint8_t[NCBPS];

        for (int sym = 0; sym < numSymbols; sym++) {
            int start = sym * NCBPS;
            memcpy(symIn, inBits + start, NCBPS);
            deinterleave_block(symIn, symOut, NCBPS, NBPSC);
            memcpy(outBits + start, symOut, NCBPS);
        }
        // Remaining bits (partial symbol): copy unchanged
        int processed = numSymbols * NCBPS;
        if (processed < numBits)
            memcpy(outBits + processed, inBits + processed, numBits - processed);

        // Pack bits back to bytes, write to out[0] at offset 3
        for (int byte = 0; byte < lip; byte++) {
            uint8_t b = 0;
            for (int bit = 0; bit < 8; bit++)
                b |= (outBits[byte * 8 + bit] & 1) << bit;
            dataOutBuf[dataOutOff + 3 + byte] = (int8_t)((int32_t)b - 128);
        }
        // bytes [3+lip..] remain 0 from memset

        delete[] inBits;
        delete[] outBits;
        delete[] symIn;
        delete[] symOut;
    }

    outData.write(dataOutBuf, actualCount);
    customData.frameCount += actualCount;

    delete[] signalBuf;
    delete[] dataBuf;
    delete[] sigOutBuf;
    delete[] dataOutBuf;
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr,
            "Usage: deinterleaver <pipeInData> <pipeInSignal> <pipeOutData> <pipeOutSignal>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1], argv[2]};
    const char* pipeOuts[] = {argv[3], argv[4]};

    BlockConfig config = {
        "Deinterleaver",
        2,                 // inputs
        2,                 // outputs
        {3027, 6},         // inputPacketSizes  [rate+lip+DATA_INT(3027), SIGNAL_INT(6)]
        {64, 64},      // inputBatchSizes
        {3027, 6},         // outputPacketSizes [rate+lip+DATA_DEINT(3027), SIGNAL(6)]
        {64, 64},      // outputBatchSizes
        true,              // ltr
        true,              // startWithAll
        "Deinterleaver: SIGNAL deint+sent first; DATA deint up to lip bytes; rate+lip passed through"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_deinterleaver, init_deinterleaver);
    return 0;
}

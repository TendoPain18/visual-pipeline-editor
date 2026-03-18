#include "core/run_generic_block.h"
#include <cstring>
#include <cstdint>

// ============================================================
// IEEE 802.11a Deinterleaver (inverse of interleaver)
//
// Inputs:
//   in[0]: rate + lip_bits + DATA_INTERLEAVED  (3029 bytes/pkt)
//   in[1]: SIGNAL_INTERLEAVED                  (6 bytes/pkt) — arrives FIRST
//
// Outputs:
//   out[0]: rate + lip_bits + DATA_DEINTERLEAVED (3029 bytes/pkt)
//   out[1]: SIGNAL_DEINTERLEAVED                 (6 bytes/pkt)
//
// Protocol (deadlock-free):
//   1. Read SIGNAL (in[1]) -- arrives first
//   2. Deinterleave + send SIGNAL (out[1])
//   3. Read DATA (in[0])
//   4. Deinterleave DATA for exact lip_bits
//   5. Send rate+lip_bits+DATA_DEINT (out[0])
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

// Inverse of the forward interleave:
// Build forward map fwd[k] = j, then use it as a lookup (out[k] = in[fwd[k]])
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

    return data;
}

void process_deinterleaver(
    const char** pipeIn,
    const char** pipeOut,
    DeinterleaverData& customData,
    const BlockConfig& config
) {
    PipeIO inData   (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);  // 3029
    PipeIO inSignal (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);  // 6
    PipeIO outData  (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]); // 3029
    PipeIO outSignal(pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]); // 6

    int8_t* signalBuf  = new int8_t[inSignal.getBufferSize()];
    int8_t* dataBuf    = new int8_t[inData.getBufferSize()];
    int8_t* sigOutBuf  = new int8_t[outSignal.getBufferSize()];
    int8_t* dataOutBuf = new int8_t[outData.getBufferSize()];

    const int inSigPkt   = config.inputPacketSizes[1];   // 6
    const int inDataPkt  = config.inputPacketSizes[0];   // 3029
    const int outSigPkt  = config.outputPacketSizes[1];  // 6
    const int outDataPkt = config.outputPacketSizes[0];  // 3029

    const bool isFirstBatch = (customData.frameCount == 0);

    // STEP 1: Read SIGNAL -- arrives first
    int actualCount = inSignal.read(signalBuf);

    memset(sigOutBuf,  0, outSignal.getBufferSize());
    memset(dataOutBuf, 0, outData.getBufferSize());

    // STEP 2: Deinterleave SIGNAL (BPSK, NCBPS=48) and send immediately
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

    // STEP 3: Send SIGNAL -- ppdu_decap parses it, sends feedback
    outSignal.write(sigOutBuf, actualCount);

    // STEP 4: Read DATA -- arrives after middleman gets feedback
    inData.read(dataBuf);



    // STEP 5+6: Deinterleave DATA for each packet
    for (int i = 0; i < actualCount; i++) {
        const bool dbg = isFirstBatch && (i == 0);

        const int dataOff    = i * inDataPkt;
        const int dataOutOff = i * outDataPkt;

        // Read header: rate(1) + lip_bits(4B LE) - EXACT bits
        uint8_t rateVal = (uint8_t)((int32_t)dataBuf[dataOff + 0] + 128);
        uint8_t b1      = (uint8_t)((int32_t)dataBuf[dataOff + 1] + 128);
        uint8_t b2      = (uint8_t)((int32_t)dataBuf[dataOff + 2] + 128);
        uint8_t b3      = (uint8_t)((int32_t)dataBuf[dataOff + 3] + 128);
        uint8_t b4      = (uint8_t)((int32_t)dataBuf[dataOff + 4] + 128);
        uint32_t lipBits = (uint32_t)b1 | ((uint32_t)b2 << 8)
                         | ((uint32_t)b3 << 16) | ((uint32_t)b4 << 24);

        int lipBitCount  = (int)lipBits;  // EXACT bits
        int lipByteCount = (lipBitCount + 7) / 8;
        if (lipByteCount > 3024) lipByteCount = 3024;

        // Passthrough header (5 bytes)
        for (int j = 0; j < 5; j++)
            dataOutBuf[dataOutOff + j] = dataBuf[dataOff + j];

        // Read DATA bytes into uint8 (data starts at offset 5)
        uint8_t dataBytes[3024] = {};
        for (int j = 0; j < lipByteCount; j++)
            dataBytes[j] = (uint8_t)((int32_t)dataBuf[dataOff + 5 + j] + 128);

        RateModParams mp = getModParams(rateVal);
        int NCBPS = mp.NCBPS, NBPSC = mp.NBPSC;

        uint8_t* inBits  = new uint8_t[lipBitCount]();
        uint8_t* outBits = new uint8_t[lipBitCount]();

        for (int bit = 0; bit < lipBitCount; bit++)
            inBits[bit] = (dataBytes[bit / 8] >> (bit % 8)) & 1;

        int numCompleteSymbols = lipBitCount / NCBPS;
        uint8_t* symIn  = new uint8_t[NCBPS];
        uint8_t* symOut = new uint8_t[NCBPS];

        for (int sym = 0; sym < numCompleteSymbols; sym++) {
            int start = sym * NCBPS;
            for (int b = 0; b < NCBPS; b++) symIn[b] = inBits[start + b];
            deinterleave_block(symIn, symOut, NCBPS, NBPSC);
            for (int b = 0; b < NCBPS; b++) outBits[start + b] = symOut[b];
        }

        // Partial last symbol
        int processed = numCompleteSymbols * NCBPS;
        if (processed < lipBitCount) {
            int partialBits = lipBitCount - processed;
            for (int b = 0; b < partialBits; b++) symIn[b] = inBits[processed + b];
            for (int b = partialBits; b < NCBPS; b++) symIn[b] = 0;
            deinterleave_block(symIn, symOut, NCBPS, NBPSC);
            for (int b = 0; b < partialBits; b++) outBits[processed + b] = symOut[b];
        }

        // Pack bits -> bytes at offset 5
        for (int byte = 0; byte < lipByteCount; byte++) {
            uint8_t bval = 0;
            for (int bit = 0; bit < 8; bit++) {
                int idx = byte * 8 + bit;
                if (idx < lipBitCount) bval |= (outBits[idx] & 1) << bit;
            }
            dataOutBuf[dataOutOff + 5 + byte] = (int8_t)((int32_t)bval - 128);
        }

        if (dbg) {
            int dataEncBits = lipBitCount - 48;
            printf("[Deinterleaver] pkt[0] INPUT : signal=48  data=%d  total=%d bits\n",
                   dataEncBits, lipBitCount);
            printf("[Deinterleaver] pkt[0] OUTPUT: signal=48  data=%d  total=%d bits\n",
                   dataEncBits, lipBitCount);
            fflush(stdout);
        }

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
            "Usage: deinterleaver <pipeInData> <pipeInSignal>"
            " <pipeOutData> <pipeOutSignal>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1], argv[2]};
    const char* pipeOuts[] = {argv[3], argv[4]};

    BlockConfig config = {
        "Deinterleaver",
        2,                 // inputs
        2,                 // outputs
        {3029, 6},         // inputPacketSizes  [rate+lip_bits(uint32 LE)+DATA_INT(3024)=3029, SIGNAL_INT(6)]
        {64, 64},          // inputBatchSizes
        {3029, 6},         // outputPacketSizes [rate+lip_bits(uint32 LE)+DATA_DEINT(3024)=3029, SIGNAL(6)]
        {64, 64},          // outputBatchSizes
        true,              // ltr
        true,              // startWithAll
        "Deinterleaver: exact bit counts (including fractional bytes)"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_deinterleaver, init_deinterleaver);
    return 0;
}
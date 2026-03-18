#include "core/run_generic_block.h"
#include <cstring>

// ============================================================
// IEEE 802.11a Descrambler
//
// Inputs:
//   in[0]: lip_bits + Scrambled DATA  (1519 bytes/pkt)
//             Bytes 0-3: lip_bits (uint32 LE, EXACT PPDU frame bits)
//             Bytes [4..4+lip_bytes-1]: scrambled PPDU
//   in[1]: SIGNAL  (3 bytes/pkt, decoded) — arrives FIRST
//
// Outputs:
//   out[0]: lip_bits + Descrambled DATA  (1519 bytes/pkt)
//             Bytes 0-3: lip_bits (passthrough)
//             Bytes [4..]: descrambled PPDU
//   out[1]: SIGNAL passthrough (3 bytes/pkt)
//
// PPDU layout (starting at in[0] byte 4):
//   Bits  0–23   = SIGNAL field -> copied unchanged
//   Bits 24+     = SERVICE+PSDU+TAIL_PAD -> descrambled
//
// Protocol (deadlock-free):
//   1. Read SIGNAL (in[1])   -- arrives first
//   2. Send SIGNAL (out[1])  -- unblocks ppdu_decap
//   3. Read DATA   (in[0])   -- arrives after middleman gets feedback
//   4. Descramble and send (out[0])
// ============================================================

struct DescramblerData {
    int     frameCount;
    uint8_t lut[128][1515];
};

DescramblerData init_descrambler(const BlockConfig& config) {
    DescramblerData data;
    data.frameCount = 0;



    for (int seed = 1; seed <= 127; seed++) {
        uint8_t st[7];
        for (int b = 0; b < 7; b++) st[b] = (seed >> (6 - b)) & 1;
        for (int bi = 0; bi < 1515; bi++) {
            uint8_t sb = 0;
            for (int bit = 0; bit < 8; bit++) {
                sb |= (st[6] << bit);
                uint8_t nb = st[6] ^ st[3];
                for (int s = 6; s > 0; s--) st[s] = st[s - 1];
                st[0] = nb;
            }
            data.lut[seed][bi] = sb;
        }
    }


    return data;
}

void process_descrambler(
    const char** pipeIn,
    const char** pipeOut,
    DescramblerData& customData,
    const BlockConfig& config
) {
    PipeIO inData   (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);  // 1519
    PipeIO inSignal (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);  // 3
    PipeIO outData  (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]); // 1519
    PipeIO outSignal(pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]); // 3

    int8_t* signalBuf  = new int8_t[inSignal.getBufferSize()];
    int8_t* dataBuf    = new int8_t[inData.getBufferSize()];
    int8_t* sigOutBuf  = new int8_t[outSignal.getBufferSize()];
    int8_t* dataOutBuf = new int8_t[outData.getBufferSize()];

    const int inDataPkt  = config.inputPacketSizes[0];   // 1519
    const int outDataPkt = config.outputPacketSizes[0];  // 1519

    const bool isFirstBatch = (customData.frameCount == 0);

    // STEP 1: Read SIGNAL -- arrives first
    int actualCount = inSignal.read(signalBuf);

    // STEP 2: Pass SIGNAL through immediately
    memcpy(sigOutBuf, signalBuf, outSignal.getBufferSize());
    outSignal.write(sigOutBuf, actualCount);

    // STEP 3: Read DATA -- arrives after middleman gets feedback
    inData.read(dataBuf);

    memset(dataOutBuf, 0, outData.getBufferSize());



    for (int i = 0; i < actualCount; i++) {
        const bool dbg = isFirstBatch && (i == 0);

        const int inOff  = i * inDataPkt;
        const int outOff = i * outDataPkt;

        // Read lip_bits from header bytes [0..3] - EXACT bits
        uint8_t b0 = (uint8_t)((int32_t)dataBuf[inOff + 0] + 128);
        uint8_t b1 = (uint8_t)((int32_t)dataBuf[inOff + 1] + 128);
        uint8_t b2 = (uint8_t)((int32_t)dataBuf[inOff + 2] + 128);
        uint8_t b3 = (uint8_t)((int32_t)dataBuf[inOff + 3] + 128);
        uint32_t lipBits = (uint32_t)b0 | ((uint32_t)b1 << 8)
                         | ((uint32_t)b2 << 16) | ((uint32_t)b3 << 24);

        int lipBitCount  = (int)lipBits;  // EXACT bits
        int lipByteCount = (lipBitCount + 7) / 8;
        if (lipByteCount > 1515) lipByteCount = 1515;

        // Passthrough 4-byte lip header
        dataOutBuf[outOff + 0] = dataBuf[inOff + 0];
        dataOutBuf[outOff + 1] = dataBuf[inOff + 1];
        dataOutBuf[outOff + 2] = dataBuf[inOff + 2];
        dataOutBuf[outOff + 3] = dataBuf[inOff + 3];

        // Read PPDU payload (starts at offset 4)
        uint8_t pkt[1515] = {};
        for (int j = 0; j < lipByteCount; j++)
            pkt[j] = (uint8_t)((int32_t)dataBuf[inOff + 4 + j] + 128);

        // Extract bits
        uint8_t* inBits  = new uint8_t[lipBitCount];
        uint8_t* outBits = new uint8_t[lipBitCount];
        for (int bit = 0; bit < lipBitCount; bit++)
            inBits[bit] = (pkt[bit / 8] >> (bit % 8)) & 1;

        // SIGNAL field: bits 0–23 -> passthrough
        const int SIGNAL_BITS = 24;
        for (int b = 0; b < SIGNAL_BITS && b < lipBitCount; b++)
            outBits[b] = inBits[b];

        // Extract seed from SERVICE byte (bits 24–30, 7 bits at byte 3 bits 0–6)
        uint8_t seed = 0;
        if (lipBitCount > 24) {
            for (int b = 0; b < 7 && (24 + b) < lipBitCount; b++) {
                int globalBit = 24 + b;
                seed |= ((pkt[globalBit / 8] >> (globalBit % 8)) & 1) << b;
            }
        }
        if (seed == 0) seed = 1;

        const uint8_t* seq = customData.lut[seed];

        // Descramble bits 24..lipBitCount-1
        if (lipBitCount > SIGNAL_BITS) {
            for (int bit = 0; bit < (lipBitCount - SIGNAL_BITS); bit++) {
                int globalBit = SIGNAL_BITS + bit;
                int seqBit = (seq[globalBit / 8] >> (globalBit % 8)) & 1;
                outBits[globalBit] = inBits[globalBit] ^ seqBit;
            }
        }

        // Repack bits -> bytes
        uint8_t out[1515] = {};
        for (int byteIdx = 0; byteIdx < lipByteCount; byteIdx++) {
            out[byteIdx] = 0;
            for (int bit = 0; bit < 8; bit++) {
                int globalBit = byteIdx * 8 + bit;
                if (globalBit < lipBitCount)
                    out[byteIdx] |= (outBits[globalBit] & 1) << bit;
            }
        }

        // Write descrambled PPDU at offset 4
        for (int j = 0; j < lipByteCount; j++)
            dataOutBuf[outOff + 4 + j] = (int8_t)((int32_t)out[j] - 128);

        if (dbg) {
            int dataBitCount = lipBitCount - SIGNAL_BITS;
            printf("[Descrambler] pkt[0] INPUT : signal=%d  data=%d  total=%d bits\n",
                   SIGNAL_BITS, dataBitCount, lipBitCount);
            printf("[Descrambler] pkt[0] OUTPUT: signal=%d  data=%d  total=%d bits\n",
                   SIGNAL_BITS, dataBitCount, lipBitCount);
            fflush(stdout);
        }

        delete[] inBits;
        delete[] outBits;
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
            "Usage: descrambler <pipeInData> <pipeInSignal>"
            " <pipeOutData> <pipeOutSignal>\n");
        return 1;
    }
    const char* pipeIns[]  = {argv[1], argv[2]};
    const char* pipeOuts[] = {argv[3], argv[4]};

    BlockConfig config = {
        "Descrambler",
        2,               // inputs
        2,               // outputs
        {1519, 3},       // inputPacketSizes  [lip_bits(uint32 LE)+DATA(1515)=1519@[0], SIGNAL(3)@[1]]
        {64, 64},        // inputBatchSizes
        {1519, 3},       // outputPacketSizes [lip_bits(uint32 LE)+DATA(1515)=1519@[0], SIGNAL(3)@[1]]
        {64, 64},        // outputBatchSizes
        true,            // ltr
        true,            // startWithAll
        "Descrambler: exact bit counts (including fractional bytes)"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_descrambler, init_descrambler);
    return 0;
}
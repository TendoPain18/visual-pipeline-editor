#include "core/run_generic_block.h"
#include <cstring>

// ============================================================
// IEEE 802.11a Descrambler
//
// Inputs:
//   in[0]: lip + Scrambled DATA  (1517 bytes/pkt)  <- [0] rate counter
//             Byte 0: lip_lo    (PPDU frame bytes low)
//             Byte 1: lip_hi    (PPDU frame bytes high)
//             Bytes [2..1516]: scrambled PPDU data (SIGNAL+SERVICE+PSDU+TAIL_PAD)
//   in[1]: SIGNAL  (3 bytes/pkt, decoded, plain)
//
// Outputs:
//   out[0]: lip + Descrambled DATA  (1517 bytes/pkt)  <- [0] rate counter
//             Byte 0: lip_lo    (passthrough)
//             Byte 1: lip_hi    (passthrough)
//             Bytes [2..2+lip-1]: descrambled PPDU (SIGNAL+SERVICE+PSDU+TAIL_PAD)
//   out[1]: SIGNAL passthrough (3 bytes/pkt)  -> ppdu_decapsulate
//
// lip = PPDU frame bytes = number of meaningful bytes in the data payload.
// Only PPDU bytes [0..lip-1] (at in[0] offsets [2..2+lip-1]) are descrambled.
//
// PPDU layout inside the data payload (from offset 2 of in[0]):
//   Bytes [0..2]   = SIGNAL field  -> copied unchanged (not scrambled)
//   Byte  [3]      = SERVICE[0]    -> bits [6:0] = scrambler seed
//   Bytes [3..lip-1] = SERVICE+PSDU+TAIL_PAD -> descrambled
//
// Protocol (deadlock-free):
//   1. Read SIGNAL (in[1])    -- SIGNAL physically arrives FIRST
//   2. Send SIGNAL (out[1])   -- ppdu_decap reads it, sends feedback to middleman,
//                                middleman then sends DATA to us
//   3. Read DATA (in[0])      -- DATA arrives after middleman got feedback
//   4. Extract lip from bytes [0..1]; descramble PPDU bytes [0..lip-1]
//   5. Send lip + descrambled DATA (out[0])
//
// Scrambler polynomial: S(x) = x^7 + x^4 + 1
// ============================================================

struct DescramblerData {
    int     frameCount;
    uint8_t lut[128][1515];
};

DescramblerData init_descrambler(const BlockConfig& config) {
    DescramblerData data;
    data.frameCount = 0;

    printf("[Descrambler] Polynomial: S(x) = x^7 + x^4 + 1\n");
    printf("[Descrambler] Building LUT for seeds 1..127 (1515 bytes each)...\n");

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

    printf("[Descrambler] LUT built\n");
    printf("[Descrambler] in[0]: [lip_lo(1)|lip_hi(1)|DATA(1515)] = 1517B\n");
    printf("[Descrambler] lip read directly from in[0] header -- no SIGNAL parse needed\n");
    printf("[Descrambler] Protocol (deadlock-free):\n");
    printf("[Descrambler]   1. Read SIGNAL (in[1])  -- arrives first\n");
    printf("[Descrambler]   2. Send SIGNAL (out[1]) -- ppdu_decap unblocks\n");
    printf("[Descrambler]   3. Read DATA   (in[0])  -- arrives after middleman gets feedback\n");
    printf("[Descrambler]   4. Descramble up to lip bytes, send lip+DATA (out[0])\n");
    printf("[Descrambler] Ready\n");

    return data;
}

void process_descrambler(
    const char** pipeIn,
    const char** pipeOut,
    DescramblerData& customData,
    const BlockConfig& config
) {
    PipeIO inData   (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);  // 1517
    PipeIO inSignal (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);  // 3
    PipeIO outData  (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]); // 1517
    PipeIO outSignal(pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]); // 3

    int8_t* signalBuf  = new int8_t[inSignal.getBufferSize()];
    int8_t* dataBuf    = new int8_t[inData.getBufferSize()];
    int8_t* sigOutBuf  = new int8_t[outSignal.getBufferSize()];
    int8_t* dataOutBuf = new int8_t[outData.getBufferSize()];

    const int inDataPkt  = config.inputPacketSizes[0];   // 1517
    const int outDataPkt = config.outputPacketSizes[0];  // 1517

    // ===== STEP 1: Read SIGNAL -- arrives first =====
    int actualCount = inSignal.read(signalBuf);

    // ===== STEP 2: Pass SIGNAL through immediately =====
    memcpy(sigOutBuf, signalBuf, outSignal.getBufferSize());
    outSignal.write(sigOutBuf, actualCount);

    // ===== STEP 3: Read DATA -- arrives after middleman gets feedback =====
    inData.read(dataBuf);

    // ===== STEP 4: Descramble each packet =====
    memset(dataOutBuf, 0, outData.getBufferSize());

    for (int i = 0; i < actualCount; i++) {
        const int inOff  = i * inDataPkt;
        const int outOff = i * outDataPkt;

        // Read lip from header bytes [0..1]
        uint8_t lipLo = (uint8_t)((int32_t)dataBuf[inOff + 0] + 128);
        uint8_t lipHi = (uint8_t)((int32_t)dataBuf[inOff + 1] + 128);
        int lip = (int)lipLo | ((int)lipHi << 8);
        if (lip > 1515) lip = 1515;
        if (lip < 5)    lip = 5;   // minimum: SIGNAL(3) + SERVICE(2)

        // Passthrough lip header to output
        dataOutBuf[outOff + 0] = dataBuf[inOff + 0];
        dataOutBuf[outOff + 1] = dataBuf[inOff + 1];

        // PPDU data starts at in[0][2]; read 'lip' meaningful bytes
        uint8_t pkt[1515] = {};
        for (int j = 0; j < lip; j++)
            pkt[j] = (uint8_t)((int32_t)dataBuf[inOff + 2 + j] + 128);

        // Extract seed from PPDU SERVICE byte 0 (PPDU byte [3]) bits [6:0]
        uint8_t seed = pkt[3] & 0x7F;
        if (seed == 0) seed = 1;

        const uint8_t* seq = customData.lut[seed];

        // PPDU bytes [0..2] = SIGNAL field: copy unchanged (not scrambled)
        uint8_t out[1515] = {};
        out[0] = pkt[0];
        out[1] = pkt[1];
        out[2] = pkt[2];

        // Descramble PPDU bytes [3..lip-1]
        for (int j = 3; j < lip; j++)
            out[j] = pkt[j] ^ seq[j];

        // Write descrambled PPDU to out[0] starting at offset 2
        for (int j = 0; j < lip; j++)
            dataOutBuf[outOff + 2 + j] = (int8_t)((int32_t)out[j] - 128);
    }

    // ===== STEP 5: Send lip + descrambled DATA =====
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
            "Usage: descrambler <pipeInData> <pipeInSignal> <pipeOutData> <pipeOutSignal>\n");
        return 1;
    }
    const char* pipeIns[]  = {argv[1], argv[2]};
    const char* pipeOuts[] = {argv[3], argv[4]};

    BlockConfig config = {
        "Descrambler",
        2,               // inputs
        2,               // outputs
        {1517, 3},       // inputPacketSizes  [lip+DATA(1517)@[0], SIGNAL(3)@[1]]
        {6000, 6000},    // inputBatchSizes
        {1517, 3},       // outputPacketSizes [lip+DATA(1517)@[0], SIGNAL(3)@[1]]
        {6000, 6000},    // outputBatchSizes
        true,            // ltr
        true,            // startWithAll
        "Descrambler: SIGNAL passthrough; lip from header; DATA descrambled up to lip bytes"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_descrambler, init_descrambler);
    return 0;
}

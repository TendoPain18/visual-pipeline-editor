#include "core/run_generic_block.h"
#include <cstring>

// ============================================================
// IEEE 802.11a PPDU Decapsulator
//
// Inputs:
//   in[0]: lip_bits + Descrambled DATA  (1519 bytes/pkt)
//             Bytes 0-3: lip_bits (uint32 LE, EXACT PPDU frame bits)
//             Bytes [4..4+lip_bytes-1]: descrambled PPDU
//   in[1]: SIGNAL  (3 bytes/pkt, decoded) — arrives FIRST
//
// Outputs:
//   out[0]: PSDU      (1504 bytes/pkt)
//   out[1]: Feedback  (3 bytes/pkt)
//             Byte 0: rate_value
//             Byte 1: mac_len_lo
//             Byte 2: mac_len_hi
//
// in[0] layout: [lip_bits(4B)] [SIGNAL(3B)] [SERVICE(2B)] [PSDU...] [TAIL_PAD]
// PSDU offset inside in[0] = 4 (lip header) + 5 (SIGNAL+SERVICE) = 9
//
// Protocol (deadlock-free):
//   1. Read SIGNAL (in[1])    -- arrives first
//   2. Parse rate + mac_length from SIGNAL
//   3. Send feedback (out[1]) -- unblocks upstream middleman
//   4. Read DATA (in[0])      -- arrives after middleman gets feedback
//   5. Slice PSDU using exact lip_bits, send (out[0])
// ============================================================

static const uint8_t VALID_RATES[] = {13, 15, 5, 7, 9, 11, 1, 3};
static const int     NUM_VALID_RATES = 8;

static bool isValidRate(uint8_t r) {
    for (int i = 0; i < NUM_VALID_RATES; i++) if (VALID_RATES[i] == r) return true;
    return false;
}

static const char* rateNameFromVal(uint8_t rateVal) {
    switch (rateVal) {
        case 13: return "6 Mbps"; case 15: return "9 Mbps";
        case  5: return "12 Mbps"; case  7: return "18 Mbps";
        case  9: return "24 Mbps"; case 11: return "36 Mbps";
        case  1: return "48 Mbps"; case  3: return "54 Mbps";
        default: return "UNKNOWN";
    }
}

struct PpduDecapsulateData { int frameCount; };

PpduDecapsulateData init_ppdu_decapsulate(const BlockConfig& config) {
    PpduDecapsulateData d;
    d.frameCount = 0;

    return d;
}

void process_ppdu_decapsulate(
    const char** pipeIn,
    const char** pipeOut,
    PpduDecapsulateData& customData,
    const BlockConfig& config
) {
    PipeIO inData     (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);  // 1519
    PipeIO inSignal   (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);  // 3
    PipeIO outPsdu    (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]); // 1504
    PipeIO outFeedback(pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]); // 3

    int8_t* signalBuf   = new int8_t[inSignal.getBufferSize()];
    int8_t* dataBuf     = new int8_t[inData.getBufferSize()];
    int8_t* feedbackBuf = new int8_t[outFeedback.getBufferSize()];
    int8_t* psduBuf     = new int8_t[outPsdu.getBufferSize()];

    const int inSigPkt   = config.inputPacketSizes[1];   // 3
    const int inDataPkt  = config.inputPacketSizes[0];   // 1519
    const int outFbPkt   = config.outputPacketSizes[1];  // 3
    const int outPsduPkt = config.outputPacketSizes[0];  // 1504

    const bool isFirstBatch = (customData.frameCount == 0);

    // STEP 1: Read SIGNAL -- arrives first
    int actualCount = inSignal.read(signalBuf);

    memset(feedbackBuf, 0, outFeedback.getBufferSize());
    memset(psduBuf,     0, outPsdu.getBufferSize());



    // STEP 2: Parse SIGNAL -> rate + mac_length
    for (int i = 0; i < actualCount; i++) {
        const int sigOff = i * inSigPkt;
        const int fbOff  = i * outFbPkt;

        uint8_t b0 = (uint8_t)((int32_t)signalBuf[sigOff + 0] + 128);
        uint8_t b1 = (uint8_t)((int32_t)signalBuf[sigOff + 1] + 128);
        uint8_t b2 = (uint8_t)((int32_t)signalBuf[sigOff + 2] + 128);

        uint8_t  rateVal = b0 & 0x0F;
        uint16_t length  = (uint16_t)((b0 >> 5) & 0x07)
                         | ((uint16_t)b1 << 3)
                         | (((uint16_t)(b2 & 0x01)) << 11);

        if (!isValidRate(rateVal)) {
            fprintf(stderr, "[PpduDecapsulate] WARNING pkt %d: invalid RATE %d -> using 5\n",
                    i, rateVal);
            rateVal = 5;
        }
        if (length == 0 || length > 4095) {
            fprintf(stderr, "[PpduDecapsulate] WARNING pkt %d: invalid LENGTH %d -> using 1504\n",
                    i, length);
            length = 1504;
        }

        feedbackBuf[fbOff + 0] = (int8_t)((int32_t)rateVal                - 128);
        feedbackBuf[fbOff + 1] = (int8_t)((int32_t)(length & 0xFF)        - 128);
        feedbackBuf[fbOff + 2] = (int8_t)((int32_t)((length >> 8) & 0xFF) - 128);

        if (isFirstBatch && i == 0) {
            // will print after data extraction
        }
    }

    // STEP 3: Send feedback -- unblocks upstream middleman
    outFeedback.write(feedbackBuf, actualCount);

    // STEP 4: Read DATA -- arrives after middleman gets feedback
    inData.read(dataBuf);

    // STEP 5: Extract PSDU using EXACT lip_bits
    for (int i = 0; i < actualCount; i++) {
        const int dataOff = i * inDataPkt;
        const int psduOff = i * outPsduPkt;

        // Read lip_bits from header bytes [0..3] - EXACT bits
        uint8_t b0 = (uint8_t)((int32_t)dataBuf[dataOff + 0] + 128);
        uint8_t b1 = (uint8_t)((int32_t)dataBuf[dataOff + 1] + 128);
        uint8_t b2 = (uint8_t)((int32_t)dataBuf[dataOff + 2] + 128);
        uint8_t b3 = (uint8_t)((int32_t)dataBuf[dataOff + 3] + 128);
        uint32_t lipBits = (uint32_t)b0 | ((uint32_t)b1 << 8)
                         | ((uint32_t)b2 << 16) | ((uint32_t)b3 << 24);

        int lipBitCount  = (int)lipBits;  // EXACT bits
        int lipByteCount = (lipBitCount + 7) / 8;

        if (lipByteCount > 1515) lipByteCount = 1515;
        if (lipByteCount < 5)    lipByteCount = 5;

        // PSDU = lip_bytes - 5 (SIGNAL 3B + SERVICE 2B), capped at 1504
        int psduLen = lipByteCount - 5;
        if (psduLen < 0)          psduLen = 0;
        if (psduLen > outPsduPkt) psduLen = outPsduPkt;

        // Copy from offset 9 = 4 (lip header) + 5 (SIGNAL+SERVICE)
        memcpy(psduBuf + psduOff, dataBuf + dataOff + 9, psduLen);

        if (isFirstBatch && i == 0) {
            int signalBitsIn = 24;
            int dataBitsIn   = lipBitCount - signalBitsIn;
            int outputBits   = psduLen * 8;
            printf("[PpduDecapsulate] pkt[0] INPUT : signal=%d  data=%d  total=%d bits\n",
                   signalBitsIn, dataBitsIn, lipBitCount);
            printf("[PpduDecapsulate] pkt[0] OUTPUT: %d bits (PSDU)\n", outputBits);
            fflush(stdout);
        }
    }



    // STEP 6: Send PSDU
    outPsdu.write(psduBuf, actualCount);

    customData.frameCount += actualCount;

    delete[] signalBuf;
    delete[] dataBuf;
    delete[] feedbackBuf;
    delete[] psduBuf;
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr,
            "Usage: ppdu_decapsulate <pipeInData> <pipeInSignal>"
            " <pipeOutPsdu> <pipeOutFeedback>\n");
        return 1;
    }
    const char* pipeIns[]  = {argv[1], argv[2]};
    const char* pipeOuts[] = {argv[3], argv[4]};

    BlockConfig config = {
        "PpduDecapsulate",
        2,               // inputs
        2,               // outputs
        {1519, 3},       // inputPacketSizes  [lip_bits(uint32 LE)+DATA(1515)=1519@[0], SIGNAL(3)@[1]]
        {64, 64},        // inputBatchSizes
        {1504, 3},       // outputPacketSizes [PSDU(1504)@[0], feedback(3)@[1]]
        {64, 64},        // outputBatchSizes
        true,            // ltr
        true,            // startWithAll
        "PPDU decap: exact bit counts (including fractional bytes)"
    };


    run_manual_block(pipeIns, pipeOuts, config, process_ppdu_decapsulate, init_ppdu_decapsulate);
    return 0;
}
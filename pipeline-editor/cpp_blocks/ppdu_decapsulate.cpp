#include "core/run_generic_block.h"
#include <cstring>

// ============================================================
// IEEE 802.11a PPDU Decapsulator
//
// Inputs:
//   in[0]: lip + Descrambled DATA  (1517 bytes/pkt)  <- [0] rate counter
//             Byte 0: lip_lo    (PPDU frame bytes low)
//             Byte 1: lip_hi    (PPDU frame bytes high)
//             Bytes [2..2+lip-1]: descrambled PPDU (SIGNAL+SERVICE+PSDU+TAIL_PAD)
//   in[1]: SIGNAL  (3 bytes/pkt)
//
// Outputs:
//   out[0]: PSDU      (1504 bytes/pkt)        <- [0] rate counter
//   out[1]: Feedback  (3 bytes/pkt)
//             Byte 0: rate_value
//             Byte 1: mac_len_lo
//             Byte 2: mac_len_hi
//
// lip is read directly from in[0] bytes [0..1].
// PSDU extraction: skip 5 bytes (SIGNAL[3]+SERVICE[2]) inside the PPDU payload,
// then copy (lip - 5) bytes capped at outPsduPkt (1504).
// No recomputation of frameBytes from mac_length needed here.
//
// SIGNAL is still parsed to build the feedback (rate + mac_length) for the
// upstream middleman -- that is the only use of SIGNAL in this block.
//
// Protocol (deadlock-free):
//   1. Read SIGNAL (in[1])   -- SIGNAL physically arrives FIRST
//   2. Parse each SIGNAL: extract rate + mac_length for feedback
//   3. Send feedback (out[1]) -- unblocks the upstream middleman's wait
//   4. Read DATA (in[0])     -- DATA arrives after middleman gets feedback
//   5. Read lip from in[0] header; copy PSDU = data[2+5 .. 2+lip-1]
//   6. Send PSDU (out[0])
// ============================================================

static const uint8_t VALID_RATES[] = {13, 15, 5, 7, 9, 11, 1, 3};
static const int     NUM_VALID_RATES = 8;

static bool isValidRate(uint8_t r) {
    for (int i = 0; i < NUM_VALID_RATES; i++)
        if (VALID_RATES[i] == r) return true;
    return false;
}

struct PpduDecapsulateData { int frameCount; };

PpduDecapsulateData init_ppdu_decapsulate(const BlockConfig& config) {
    PpduDecapsulateData d;
    d.frameCount = 0;
    printf("[PpduDecapsulate] in[0]: [lip_lo(1)|lip_hi(1)|DATA(1515)] = 1517B\n");
    printf("[PpduDecapsulate] lip used directly to slice PSDU (no frameBytes recompute)\n");
    printf("[PpduDecapsulate] Protocol (deadlock-free):\n");
    printf("[PpduDecapsulate]   1. Read SIGNAL   (in[1]) -- arrives first\n");
    printf("[PpduDecapsulate]   2. Send feedback (out[1])-- unblocks middleman\n");
    printf("[PpduDecapsulate]   3. Read DATA     (in[0]) -- arrives after feedback\n");
    printf("[PpduDecapsulate]   4. Slice PSDU using lip, send (out[0])\n");
    printf("[PpduDecapsulate] Ready\n");
    return d;
}

void process_ppdu_decapsulate(
    const char** pipeIn,
    const char** pipeOut,
    PpduDecapsulateData& customData,
    const BlockConfig& config
) {
    PipeIO inData     (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);  // 1517
    PipeIO inSignal   (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);  // 3
    PipeIO outPsdu    (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]); // 1504
    PipeIO outFeedback(pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]); // 3

    int8_t* signalBuf   = new int8_t[inSignal.getBufferSize()];
    int8_t* dataBuf     = new int8_t[inData.getBufferSize()];
    int8_t* feedbackBuf = new int8_t[outFeedback.getBufferSize()];
    int8_t* psduBuf     = new int8_t[outPsdu.getBufferSize()];

    const int inSigPkt   = config.inputPacketSizes[1];   // 3
    const int inDataPkt  = config.inputPacketSizes[0];   // 1517
    const int outFbPkt   = config.outputPacketSizes[1];  // 3
    const int outPsduPkt = config.outputPacketSizes[0];  // 1504

    // ===== STEP 1: Read SIGNAL -- arrives first =====
    int actualCount = inSignal.read(signalBuf);

    memset(feedbackBuf, 0, outFeedback.getBufferSize());
    memset(psduBuf,     0, outPsdu.getBufferSize());

    // ===== STEP 2: Parse SIGNAL -> rate + mac_length (for feedback only) =====
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
            printf("[PpduDecapsulate] WARNING pkt %d: invalid RATE %d -> using 5\n", i, rateVal);
            rateVal = 5;
        }
        if (length == 0 || length > 4095) {
            printf("[PpduDecapsulate] WARNING pkt %d: invalid LENGTH %d -> using 1504\n", i, length);
            length = 1504;
        }

        feedbackBuf[fbOff + 0] = (int8_t)((int32_t)rateVal                - 128);
        feedbackBuf[fbOff + 1] = (int8_t)((int32_t)(length & 0xFF)        - 128);
        feedbackBuf[fbOff + 2] = (int8_t)((int32_t)((length >> 8) & 0xFF) - 128);
    }

    // ===== STEP 3: Send feedback -- unblocks the upstream middleman =====
    outFeedback.write(feedbackBuf, actualCount);

    // ===== STEP 4: Read DATA -- arrives after middleman gets feedback =====
    inData.read(dataBuf);

    // ===== STEP 5: Extract PSDU using lip from in[0] header =====
    // in[0] layout per pkt: [lip_lo(1) | lip_hi(1) | PPDU(lip bytes)]
    // PPDU layout:          SIGNAL(3)  + SERVICE(2) + PSDU + TAIL_PAD
    // PSDU offset inside PPDU = 5 bytes
    // PSDU offset inside in[0] = 2 (lip header) + 5 (SIGNAL+SERVICE) = 7
    for (int i = 0; i < actualCount; i++) {
        const int dataOff = i * inDataPkt;
        const int psduOff = i * outPsduPkt;

        uint8_t lipLo = (uint8_t)((int32_t)dataBuf[dataOff + 0] + 128);
        uint8_t lipHi = (uint8_t)((int32_t)dataBuf[dataOff + 1] + 128);
        int lip = (int)lipLo | ((int)lipHi << 8);
        if (lip > 1515) lip = 1515;
        if (lip < 5)    lip = 5;

        // PSDU = lip - 5 bytes (skip SIGNAL+SERVICE), capped at 1504
        int psduLen = lip - 5;
        if (psduLen < 0)         psduLen = 0;
        if (psduLen > outPsduPkt) psduLen = outPsduPkt;

        // Copy from in[0] offset 7 (= 2 header + 5 SIGNAL+SERVICE)
        memcpy(psduBuf + psduOff, dataBuf + dataOff + 7, psduLen);
    }

    // ===== STEP 6: Send PSDU =====
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
        {1517, 3},       // inputPacketSizes  [lip+DATA(1517)@[0], SIGNAL(3)@[1]]
        {6000, 6000},    // inputBatchSizes
        {1504, 3},       // outputPacketSizes [PSDU(1504)@[0], feedback(3)@[1]]
        {6000, 6000},    // outputBatchSizes
        true,            // ltr
        true,            // startWithAll
        "PPDU decap: SIGNAL->feedback->DATA; PSDU sliced using lip from in[0] header"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_ppdu_decapsulate, init_ppdu_decapsulate);
    return 0;
}

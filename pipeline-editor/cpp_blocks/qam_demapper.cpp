#include "core/run_generic_block.h"
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>

// ============================================================
// IEEE 802.11a QAM Demapper with OFDM Symbol Extraction
//
// Inputs:
//   in[0]: OFDM symbols  (256 bytes/symbol = 64 subcarriers × 4 bytes)  <- FIRST
//             Per subcarrier: I[2 bytes int16 LE] + Q[2 bytes int16 LE]
//   in[1]: Feedback from ppdu_decapsulate  (3 bytes/pkt)
//             Byte 0: rate_value
//             Byte 1: mac_len_lo
//             Byte 2: mac_len_hi
//
// Outputs:
//   out[0]: SIGNAL bits  (6 bytes = 48 bits demapped from first OFDM symbol)  <- FIRST
//   out[1]: rate + DATA bits  (3025 bytes/pkt)
//             Byte 0: rate_value (from feedback)
//             Bytes [1..]: demapped DATA bits
//   out[2]: Scatter plot data (variable size, sent per batch)
//             First 4 bytes: uint32 number of symbols (little-endian)
//             Remaining: Interleaved I,Q pairs as int16 (I[0],Q[0],I[1],Q[1],...)
//
// Protocol (deadlock-free):
//   1. Read SIGNAL OFDM symbol (in[0])  -- arrives FIRST
//   2. Extract 48 data symbols (no pilots/nulls), demap BPSK -> 48 bits
//   3. Send SIGNAL bits (out[0])        -- ppdu_decap reads, sends feedback
//   4. Wait for feedback (in[1])        -- gate; get rate, calculate N_SYM
//   5. Read N_SYM DATA OFDM symbols (in[0])
//   6. Extract data symbols, demap to bits
//   7. Send rate + DATA bits (out[1])
//   8. If ENABLE_PLOTTING: accumulate symbols, send batch scatter plot (out[2])
//
// OFDM Symbol Structure (64 subcarriers, IEEE 802.11a):
//   - 48 data subcarriers (included in scatter plot)
//   - 4 pilot subcarriers (NOT in scatter plot)
//   - 12 null subcarriers (NOT in scatter plot)
// ============================================================

// CONFIGURATION: Set to true to enable constellation plotting
static const bool ENABLE_PLOTTING = false;  // <-- Change here to disable plotting

struct RateModParams {
    int    NBPSC;  // bits per subcarrier
    int    NCBPS;  // coded bits per OFDM symbol
    int    NDBPS;  // data bits per OFDM symbol
    const char* modType;
};

static RateModParams getRateModParams(uint8_t rateVal) {
    switch (rateVal) {
        case 13: return {1,  48,  24, "BPSK"};   //  6 Mbps
        case 15: return {1,  48,  36, "BPSK"};   //  9 Mbps
        case  5: return {2,  96,  48, "QPSK"};   // 12 Mbps
        case  7: return {2,  96,  72, "QPSK"};   // 18 Mbps
        case  9: return {4, 192,  96, "16QAM"};  // 24 Mbps
        case 11: return {4, 192, 144, "16QAM"};  // 36 Mbps
        case  1: return {6, 288, 192, "64QAM"};  // 48 Mbps
        case  3: return {6, 288, 216, "64QAM"};  // 54 Mbps
        default: return {2,  96,  48, "QPSK"};   // fallback: 12 Mbps
    }
}

// Data subcarrier indices (48 subcarriers)
static const int DATA_SUBCARRIERS[48] = {
    -26,-25,-24,-23,-22, -20,-19,-18,-17,-16,-15,-14,-13,-12,-11,-10,-9,-8,
    -6,-5,-4,-3,-2,-1, 1,2,3,4,5,6, 8,9,10,11,12,13,14,15,16,17,18,19,20,
    22,23,24,25,26
};

// Extract 64 complex subcarriers from packed int8 format
static void extractSubcarriers(const int8_t* ofdmSymbol, double* subcarriersI, double* subcarriersQ) {
    for (int i = 0; i < 64; i++) {
        int baseIdx = i * 4;
        
        // Convert int8 to uint8
        uint8_t I_bytes[2], Q_bytes[2];
        I_bytes[0] = (uint8_t)((int16_t)ofdmSymbol[baseIdx + 0] + 128);
        I_bytes[1] = (uint8_t)((int16_t)ofdmSymbol[baseIdx + 1] + 128);
        Q_bytes[0] = (uint8_t)((int16_t)ofdmSymbol[baseIdx + 2] + 128);
        Q_bytes[1] = (uint8_t)((int16_t)ofdmSymbol[baseIdx + 3] + 128);
        
        // Reconstruct int16 values
        int16_t I_val, Q_val;
        memcpy(&I_val, I_bytes, 2);
        memcpy(&Q_val, Q_bytes, 2);
        
        // Convert to double and descale
        subcarriersI[i] = (double)I_val / 32767.0;
        subcarriersQ[i] = (double)Q_val / 32767.0;
    }
}

// Demapping functions
static uint8_t demapBpsk(double I, double Q) {
    return (I < 0.0) ? 0 : 1;
}

static void demapQpsk(double I, double Q, uint8_t* bits) {
    bits[0] = (I < 0.0) ? 0 : 1;
    bits[1] = (Q < 0.0) ? 0 : 1;
}

static void demap16Qam(double I, double Q, uint8_t* bits) {
    const double k = 1.0 / sqrt(10.0);
    I /= k;
    Q /= k;
    
    if (I < -2.0)      bits[0] = 0, bits[1] = 0;
    else if (I < 0.0)  bits[0] = 0, bits[1] = 1;
    else if (I < 2.0)  bits[0] = 1, bits[1] = 1;
    else               bits[0] = 1, bits[1] = 0;
    
    if (Q < -2.0)      bits[2] = 0, bits[3] = 0;
    else if (Q < 0.0)  bits[2] = 0, bits[3] = 1;
    else if (Q < 2.0)  bits[2] = 1, bits[3] = 1;
    else               bits[2] = 1, bits[3] = 0;
}

static void demap64Qam(double I, double Q, uint8_t* bits) {
    const double k = 1.0 / sqrt(42.0);
    I /= k;
    Q /= k;
    
    if (I < -6.0)      bits[0] = 0, bits[1] = 0, bits[2] = 0;
    else if (I < -4.0) bits[0] = 0, bits[1] = 0, bits[2] = 1;
    else if (I < -2.0) bits[0] = 0, bits[1] = 1, bits[2] = 1;
    else if (I < 0.0)  bits[0] = 0, bits[1] = 1, bits[2] = 0;
    else if (I < 2.0)  bits[0] = 1, bits[1] = 1, bits[2] = 0;
    else if (I < 4.0)  bits[0] = 1, bits[1] = 1, bits[2] = 1;
    else if (I < 6.0)  bits[0] = 1, bits[1] = 0, bits[2] = 1;
    else               bits[0] = 1, bits[1] = 0, bits[2] = 0;
    
    if (Q < -6.0)      bits[3] = 0, bits[4] = 0, bits[5] = 0;
    else if (Q < -4.0) bits[3] = 0, bits[4] = 0, bits[5] = 1;
    else if (Q < -2.0) bits[3] = 0, bits[4] = 1, bits[5] = 1;
    else if (Q < 0.0)  bits[3] = 0, bits[4] = 1, bits[5] = 0;
    else if (Q < 2.0)  bits[3] = 1, bits[4] = 1, bits[5] = 0;
    else if (Q < 4.0)  bits[3] = 1, bits[4] = 1, bits[5] = 1;
    else if (Q < 6.0)  bits[3] = 1, bits[4] = 0, bits[5] = 1;
    else               bits[3] = 1, bits[4] = 0, bits[5] = 0;
}

struct QamDemapperData {
    int frameCount;
    std::vector<int16_t> scatterI_accum;
    std::vector<int16_t> scatterQ_accum;
};

QamDemapperData init_qam_demapper(const BlockConfig& config) {
    QamDemapperData data;
    data.frameCount = 0;
    
    printf("[QamDemapper] IEEE 802.11a QAM demapper with OFDM extraction\n");
    printf("[QamDemapper] Protocol (deadlock-free):\n");
    printf("[QamDemapper]   1. Read SIGNAL OFDM (in[0])  -- arrives first\n");
    printf("[QamDemapper]   2. Send SIGNAL bits (out[0]) -- ppdu_decap unblocks\n");
    printf("[QamDemapper]   3. Wait feedback (in[1])     -- gate; get rate, calc N_SYM\n");
    printf("[QamDemapper]   4. Read N_SYM DATA OFDM (in[0])\n");
    printf("[QamDemapper]   5. Send rate+DATA bits (out[1])\n");
    if (ENABLE_PLOTTING) {
        printf("[QamDemapper]   6. Send batch scatter plot (out[2])\n");
        printf("[QamDemapper] Constellation plotting: ENABLED\n");
    } else {
        printf("[QamDemapper] Constellation plotting: DISABLED\n");
    }
    printf("[QamDemapper] Ready\n");
    
    return data;
}

void process_qam_demapper(
    const char** pipeIn,
    const char** pipeOut,
    QamDemapperData& customData,
    const BlockConfig& config
) {
    PipeIO inOfdm     (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);
    PipeIO inFeedback (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);
    PipeIO outSignal  (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);
    PipeIO outData    (pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]);
    PipeIO outScatter (pipeOut[2], config.outputPacketSizes[2], config.outputBatchSizes[2]);
    
    int8_t* ofdmBuf     = new int8_t[256];
    int8_t* feedbackBuf = new int8_t[inFeedback.getBufferSize()];
    int8_t* signalBuf   = new int8_t[outSignal.getBufferSize()];
    int8_t* dataBuf     = new int8_t[outData.getBufferSize()];
    
    const int inFbPkt     = config.inputPacketSizes[1];   // 3
    const int outSigPkt   = config.outputPacketSizes[0];  // 6
    const int outDataPkt  = config.outputPacketSizes[1];  // 3025
    
    // ===== STEP 1: Read SIGNAL OFDM symbol (in[0]) -- arrives first =====
    inOfdm.read(ofdmBuf);
    
    // ===== STEP 2: Extract 48 data symbols, demap BPSK -> 48 bits =====
    double subcarriersI[64], subcarriersQ[64];
    extractSubcarriers(ofdmBuf, subcarriersI, subcarriersQ);
    
    uint8_t signalBits[48];
    for (int i = 0; i < 48; i++) {
        int sc_idx = DATA_SUBCARRIERS[i];
        int matlab_idx = sc_idx + 32;
        signalBits[i] = demapBpsk(subcarriersI[matlab_idx], subcarriersQ[matlab_idx]);
    }
    
    // Pack to bytes
    memset(signalBuf, 0, outSignal.getBufferSize());
    for (int byte = 0; byte < 6; byte++) {
        uint8_t b = 0;
        for (int bit = 0; bit < 8; bit++) {
            b |= (signalBits[byte * 8 + bit] << bit);  // right-MSB
        }
        signalBuf[byte] = (int8_t)((int16_t)b - 128);
    }
    
    // ===== STEP 3: Send SIGNAL bits (out[0]) -- ppdu_decap unblocks =====
    outSignal.write(signalBuf, 1);
    
    // ===== STEP 4: Wait for feedback (in[1]) -- gate =====
    int actualCount = inFeedback.read(feedbackBuf);
    
    // Process each packet in the batch
    for (int pktIdx = 0; pktIdx < actualCount; pktIdx++) {
        const int fbOff = pktIdx * inFbPkt;
        
        uint8_t rateVal = (uint8_t)((int32_t)feedbackBuf[fbOff + 0] + 128);
        uint8_t lenLo   = (uint8_t)((int32_t)feedbackBuf[fbOff + 1] + 128);
        uint8_t lenHi   = (uint8_t)((int32_t)feedbackBuf[fbOff + 2] + 128);
        uint16_t psduLength = (uint16_t)lenLo | ((uint16_t)lenHi << 8);
        
        // ===== STEP 5: Calculate N_SYM (must match mapper) =====
        RateModParams params = getRateModParams(rateVal);
        
        int L_FRAMING = 1500 * 8;
        int L_CRC     = L_FRAMING + 32;
        int L_PPDU    = 16 + L_CRC + 6;
        int N_DATA    = L_PPDU;
        int N_SYM     = (N_DATA + params.NDBPS - 1) / params.NDBPS;
        int totalDataBits = N_SYM * params.NCBPS;
        
        // ===== STEP 6: Read N_SYM DATA OFDM symbols =====
        uint8_t* dataBits = new uint8_t[totalDataBits]();
        int bitIdx = 0;
        
        for (int ofdmIdx = 0; ofdmIdx < N_SYM; ofdmIdx++) {
            inOfdm.read(ofdmBuf);
            extractSubcarriers(ofdmBuf, subcarriersI, subcarriersQ);
            
            // Extract and demap 48 data symbols
            for (int i = 0; i < 48; i++) {
                int sc_idx = DATA_SUBCARRIERS[i];
                int matlab_idx = sc_idx + 32;
                double I = subcarriersI[matlab_idx];
                double Q = subcarriersQ[matlab_idx];
                
                uint8_t symbolBits[6];
                if (strcmp(params.modType, "BPSK") == 0) {
                    symbolBits[0] = demapBpsk(I, Q);
                    if (bitIdx < totalDataBits) dataBits[bitIdx++] = symbolBits[0];
                } else if (strcmp(params.modType, "QPSK") == 0) {
                    demapQpsk(I, Q, symbolBits);
                    for (int b = 0; b < 2 && bitIdx < totalDataBits; b++)
                        dataBits[bitIdx++] = symbolBits[b];
                } else if (strcmp(params.modType, "16QAM") == 0) {
                    demap16Qam(I, Q, symbolBits);
                    for (int b = 0; b < 4 && bitIdx < totalDataBits; b++)
                        dataBits[bitIdx++] = symbolBits[b];
                } else if (strcmp(params.modType, "64QAM") == 0) {
                    demap64Qam(I, Q, symbolBits);
                    for (int b = 0; b < 6 && bitIdx < totalDataBits; b++)
                        dataBits[bitIdx++] = symbolBits[b];
                }
                
                // Accumulate symbols for scatter plot (DATA symbols only)
                if (ENABLE_PLOTTING) {
                    int16_t I_val = (int16_t)round(I * 32767.0);
                    int16_t Q_val = (int16_t)round(Q * 32767.0);
                    customData.scatterI_accum.push_back(I_val);
                    customData.scatterQ_accum.push_back(Q_val);
                }
            }
        }
        
        // ===== STEP 7: Pack bits to bytes and send =====
        int numDataBytes = (totalDataBits + 7) / 8;
        uint8_t* dataBytes = new uint8_t[numDataBytes]();
        for (int byte = 0; byte < numDataBytes; byte++) {
            uint8_t b = 0;
            for (int bit = 0; bit < 8; bit++) {
                int bitPos = byte * 8 + bit;
                if (bitPos < totalDataBits)
                    b |= (dataBits[bitPos] << bit);  // right-MSB
            }
            dataBytes[byte] = b;
        }
        
        const int dataOff = pktIdx * outDataPkt;
        memset(dataBuf + dataOff, 0, outDataPkt);
        dataBuf[dataOff + 0] = (int8_t)((int32_t)rateVal - 128);  // rate byte
        for (int i = 0; i < numDataBytes && i < outDataPkt - 1; i++) {
            dataBuf[dataOff + 1 + i] = (int8_t)((int32_t)dataBytes[i] - 128);
        }
        
        delete[] dataBits;
        delete[] dataBytes;
    }
    
    // ===== STEP 8: Send rate+DATA bits =====
    outData.write(dataBuf, actualCount);
    
    // ===== STEP 9: Send batch scatter plot (if enabled) =====
    if (ENABLE_PLOTTING && !customData.scatterI_accum.empty()) {
        uint32_t totalSymbols = (uint32_t)customData.scatterI_accum.size();
        
        // Allocate scatter buffer matching output pipe size
        int8_t* scatterBuf = new int8_t[outScatter.getBufferSize()];
        memset(scatterBuf, 0, outScatter.getBufferSize());
        
        // Write count (little-endian uint32)
        uint8_t countBytes[4];
        memcpy(countBytes, &totalSymbols, 4);
        for (int i = 0; i < 4; i++) {
            scatterBuf[i] = (int8_t)((int16_t)countBytes[i] - 128);
        }
        
        // Write interleaved I/Q pairs (up to buffer limit)
        int maxSymbols = (outScatter.getPacketSize() - 4) / 4;  // (bufferSize - header) / 4 bytes per symbol
        int symbolsToSend = (totalSymbols < maxSymbols) ? totalSymbols : maxSymbols;
        
        for (int i = 0; i < symbolsToSend; i++) {
            int16_t I_val = customData.scatterI_accum[i];
            int16_t Q_val = customData.scatterQ_accum[i];
            
            uint8_t I_bytes[2], Q_bytes[2];
            memcpy(I_bytes, &I_val, 2);
            memcpy(Q_bytes, &Q_val, 2);
            
            int baseIdx = 4 + i * 4;
            scatterBuf[baseIdx + 0] = (int8_t)((int16_t)I_bytes[0] - 128);
            scatterBuf[baseIdx + 1] = (int8_t)((int16_t)I_bytes[1] - 128);
            scatterBuf[baseIdx + 2] = (int8_t)((int16_t)Q_bytes[0] - 128);
            scatterBuf[baseIdx + 3] = (int8_t)((int16_t)Q_bytes[1] - 128);
        }
        
        // Send scatter data (one packet per batch)
        outScatter.write(scatterBuf, 1);
        
        printf("[QamDemapper] Batch %d: Sent %d DATA symbols to scatter plot\n",
               customData.frameCount + 1, symbolsToSend);
        
        delete[] scatterBuf;
        
        // Clear accumulation
        customData.scatterI_accum.clear();
        customData.scatterQ_accum.clear();
    }
    
    customData.frameCount += actualCount;
    
    delete[] ofdmBuf;
    delete[] feedbackBuf;
    delete[] signalBuf;
    delete[] dataBuf;
}

int main(int argc, char* argv[]) {
    if (argc < 6) {
        fprintf(stderr,
            "Usage: qam_demapper <pipeInOfdm> <pipeInFeedback>"
            " <pipeOutSignal> <pipeOutData> <pipeOutScatter>\n");
        return 1;
    }
    
    const char* pipeIns[]  = {argv[1], argv[2]};
    const char* pipeOuts[] = {argv[3], argv[4], argv[5]};
    
    BlockConfig config = {
        "QamDemapper",
        2,                       // inputs
        3,                       // outputs
        {256, 3},                // inputPacketSizes  [OFDM_SYMBOL, feedback]
        {6000, 6000},            // inputBatchSizes
        {6, 3025, 50000},        // outputPacketSizes [SIGNAL, rate+DATA, scatter]
        {6000, 6000, 1},         // outputBatchSizes  [signal batch, data batch, 1 scatter/batch]
        false,                   // ltr (right-to-left)
        true,                    // startWithAll
        "IEEE 802.11a QAM demapper: OFDM -> QAM symbols -> bits, with batch scatter plot"
    };
    
    run_manual_block(pipeIns, pipeOuts, config, process_qam_demapper, init_qam_demapper);
    return 0;
}
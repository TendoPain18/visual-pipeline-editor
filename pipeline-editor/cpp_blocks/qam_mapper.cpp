#include "core/run_generic_block.h"
#include <cstring>
#include <cstdint>
#include <cmath>

// ============================================================
// IEEE 802.11a QAM Mapper with OFDM Symbol Structuring
//
// Inputs:
//   in[0]: Interleaved DATA  (3030 bytes/pkt, max)         <- FIRST
//             First 6 bytes (48 bits) = SIGNAL field (always BPSK)
//             Remaining bytes = DATA field (rate-dependent modulation)
//   in[1]: rate_encoded_length  (3 bytes/pkt)
//             Byte 0: rate_value
//             Byte 1: encoded_len_lo
//             Byte 2: encoded_len_hi
//
// Outputs:
//   out[0]: OFDM symbols  (256 bytes/symbol)               <- FIRST
//             64 subcarriers × 4 bytes per subcarrier
//             Per subcarrier: I[2 bytes int16 LE] + Q[2 bytes int16 LE]
//             Scaled by 32767.
//             One SIGNAL symbol followed by N_SYM DATA symbols per frame.
//
// OFDM Symbol Structure (IEEE 802.11a, 64 subcarriers):
//   - 48 data subcarriers: -26..-22, -20..-8, -6..-1, 1..6, 8..20, 22..26
//   - 4 pilot subcarriers: -21, -7, 7, 21
//   - 12 null subcarriers: DC (0) + 11 edge guards
//
// Processing per frame (1 SIGNAL OFDM symbol + N_SYM DATA OFDM symbols):
//   1. Read interleaved DATA (in[0])  -- FIRST
//   2. Read rate_encoded_length (in[1])
//   3. Map first 48 bits to BPSK -> SIGNAL OFDM symbol -> write to out[0]
//   4. For each DATA OFDM symbol (N_SYM total):
//        map NCBPS bits with rate-dependent modulation -> write to out[0]
//
// N_SYM calculation (must match demapper):
//   L_PPDU = SERVICE_bits(16) + payload_bits(1500*8) + CRC_bits(32) + TAIL_bits(6) = 12054
//   N_SYM  = ceil(L_PPDU / NDBPS)
//
// Constellation maps (IEEE 802.11a Gray-coded):
//   BPSK  : [-1, +1]
//   QPSK  : (1/sqrt(2)) * [-1-j, -1+j, +1-j, +1+j]  (left-MSB bit index)
//   16-QAM: Gray coded, k=1/sqrt(10)
//   64-QAM: Gray coded, k=1/sqrt(42)
//
// Pilot sequence: 127-element PN sequence (cyclic), pilots are p_n * {1,1,1,-1}
//   SIGNAL symbol uses pilot_seq[0], DATA symbols use pilot_seq[ofdmIdx % 127]
//
// Rate -> (NBPSC, NCBPS, NDBPS):
//   13 ->  6 Mbps: BPSK,   NBPSC=1, NCBPS=48,  NDBPS=24
//   15 ->  9 Mbps: BPSK,   NBPSC=1, NCBPS=48,  NDBPS=36
//    5 -> 12 Mbps: QPSK,   NBPSC=2, NCBPS=96,  NDBPS=48
//    7 -> 18 Mbps: QPSK,   NBPSC=2, NCBPS=96,  NDBPS=72
//    9 -> 24 Mbps: 16-QAM, NBPSC=4, NCBPS=192, NDBPS=96
//   11 -> 36 Mbps: 16-QAM, NBPSC=4, NCBPS=192, NDBPS=144
//    1 -> 48 Mbps: 64-QAM, NBPSC=6, NCBPS=288, NDBPS=192
//    3 -> 54 Mbps: 64-QAM, NBPSC=6, NCBPS=288, NDBPS=216
// ============================================================

// ---- Rate parameters ----------------------------------------
struct RateModParams {
    int    NBPSC;
    int    NCBPS;
    int    NDBPS;
};

static RateModParams getRateModParams(uint8_t rateVal) {
    switch (rateVal) {
        case 13: return {1,  48,  24};  //  6 Mbps BPSK
        case 15: return {1,  48,  36};  //  9 Mbps BPSK
        case  5: return {2,  96,  48};  // 12 Mbps QPSK
        case  7: return {2,  96,  72};  // 18 Mbps QPSK
        case  9: return {4, 192,  96};  // 24 Mbps 16-QAM
        case 11: return {4, 192, 144};  // 36 Mbps 16-QAM
        case  1: return {6, 288, 192};  // 48 Mbps 64-QAM
        case  3: return {6, 288, 216};  // 54 Mbps 64-QAM
        default: return {2,  96,  48};  // fallback 12 Mbps QPSK
    }
}

// ---- Subcarrier layout -------------------------------------
// 48 data subcarrier indices (offsets from DC, range -32..+31 in 64-pt DFT)
static const int DATA_SUBCARRIERS[48] = {
    -26,-25,-24,-23,-22,
    -20,-19,-18,-17,-16,-15,-14,-13,-12,-11,-10,-9,-8,
    -6,-5,-4,-3,-2,-1,
    1,2,3,4,5,6,
    8,9,10,11,12,13,14,15,16,17,18,19,20,
    22,23,24,25,26
};

// 4 pilot subcarrier indices
static const int PILOT_SUBCARRIERS[4] = {-21, -7, 7, 21};

// 127-element pilot PN sequence (IEEE 802.11a Table 18-7)
static const int8_t PILOT_SEQ[127] = {
     1, 1, 1, 1,-1,-1,-1, 1,-1,-1,-1,-1, 1, 1,-1, 1,
    -1,-1, 1, 1,-1, 1, 1,-1, 1, 1, 1, 1, 1, 1,-1, 1,
     1, 1,-1, 1, 1,-1, 1,-1,-1, 1, 1, 1,-1, 1,-1,-1,
    -1, 1,-1, 1,-1,-1, 1,-1,-1, 1, 1, 1, 1, 1,-1,-1,
     1, 1,-1,-1, 1,-1, 1,-1, 1, 1,-1,-1,-1, 1, 1,-1,
    -1,-1,-1, 1,-1,-1, 1,-1, 1, 1, 1, 1,-1, 1,-1, 1,
    -1, 1,-1,-1,-1,-1,-1, 1,-1, 1, 1,-1, 1,-1, 1, 1,
     1,-1,-1, 1,-1,-1,-1,-1,-1,-1,-1
};

// ---- Constellation maps (pre-computed at init) --------------
// Using complex represented as two doubles (I, Q).
struct Complex { double I; double Q; };

// BPSK: 2 points indexed by 1 bit (left-MSB)
static Complex BPSK_MAP[2];

// QPSK: 4 points indexed by 2 bits (left-MSB -> index 0..3)
static Complex QPSK_MAP[4];

// 16-QAM: 16 points indexed by 4 bits (left-MSB -> index 0..15)
static Complex QAM16_MAP[16];

// 64-QAM: 64 points indexed by 6 bits (left-MSB -> index 0..63)
static Complex QAM64_MAP[64];

// Gray decode tables for I axis
// 16-QAM I (2 bits b0b1) -> I-level: 00=-3,01=-1,11=+1,10=+3
static const int QAM16_I_LEVEL[4] = {-3, -1, 3, 1};  // indexed by b0b1 Gray

// 16-QAM Q axis same mapping
// 64-QAM I (3 bits b0b1b2) -> I-level: 000=-7,001=-5,011=-3,010=-1,110=+1,111=+3,101=+5,100=+7
static const int QAM64_I_LEVEL[8] = {-7, -5, -3, -1, 7, 5, 3, 1};
// Index map: 0->-7, 1->-5, 2->-1(wait, see MATLAB)
// From MATLAB: b0b1b2 case 0=-7,1=-5,3=-3,2=-1,6=+1,7=+3,5=+5,4=+7
// So by b0b1b2 index: [0]=-7,[1]=-5,[2]=-1,[3]=-3,[4]=+7,[5]=+5,[6]=+1,[7]=+3
static const int QAM64_LEVEL_FROM_GRAY[8] = {-7, -5, -1, -3, 7, 5, 1, 3};

static void buildConstellations() {
    // BPSK
    BPSK_MAP[0] = {-1.0, 0.0};
    BPSK_MAP[1] = { 1.0, 0.0};

    // QPSK: k = 1/sqrt(2)
    // bits b0b1 (left-MSB): 00->-1-j, 01->-1+j, 10->+1-j, 11->+1+j
    {
        const double k = 1.0 / sqrt(2.0);
        QPSK_MAP[0] = {-k, -k};  // 00
        QPSK_MAP[1] = {-k,  k};  // 01
        QPSK_MAP[2] = { k, -k};  // 10
        QPSK_MAP[3] = { k,  k};  // 11
    }

    // 16-QAM: k = 1/sqrt(10)
    // b0b1 -> I, b2b3 -> Q  (same Gray mapping for both axes)
    // From MATLAB: b0b1: 0->-3, 1->-1, 3->+1, 2->+3
    {
        const double k = 1.0 / sqrt(10.0);
        // I levels indexed by b0b1 (Gray): 0->-3, 1->-1, 2->+3, 3->+1
        // Q levels indexed by b2b3 (Gray): same
        static const int lvl[4] = {-3, -1, 3, 1}; // gray index -> level
        for (int b = 0; b < 16; b++) {
            int b0b1 = (b >> 2) & 0x3;
            int b2b3 = b & 0x3;
            QAM16_MAP[b] = {k * lvl[b0b1], k * lvl[b2b3]};
        }
    }

    // 64-QAM: k = 1/sqrt(42)
    // b0b1b2 -> I, b3b4b5 -> Q  (Gray coded per MATLAB)
    {
        const double k = 1.0 / sqrt(42.0);
        for (int b = 0; b < 64; b++) {
            int b012 = (b >> 3) & 0x7;
            int b345 = b & 0x7;
            QAM64_MAP[b] = {k * QAM64_LEVEL_FROM_GRAY[b012],
                            k * QAM64_LEVEL_FROM_GRAY[b345]};
        }
    }
}

// ---- Symbol mapping helper ----------------------------------
// Map NBPSC bits (right-MSB order within byte, bits[0]=LSB side) to complex symbol.
// 'bits' array uses right-MSB ordering matching the interleaver output.
// For left-MSB constellation index: reverse bit order for MSB-first lookup.
static Complex mapBitsToSymbol(const uint8_t* bits, int NBPSC) {
    // Build index from bits: bits[0] is right-MSB (= LSB of byte), so for
    // left-MSB constellation index we reverse:
    // index = bits[0]*2^(NBPSC-1) + bits[1]*2^(NBPSC-2) + ... + bits[NBPSC-1]*2^0
    int idx = 0;
    for (int b = 0; b < NBPSC; b++) {
        idx = (idx << 1) | (bits[b] & 1);
    }

    switch (NBPSC) {
        case 1: return BPSK_MAP[idx & 1];
        case 2: return QPSK_MAP[idx & 3];
        case 4: return QAM16_MAP[idx & 15];
        case 6: return QAM64_MAP[idx & 63];
        default: return {0.0, 0.0};
    }
}

// ---- Pack complex symbol into 4 int8 bytes (I_lo, I_hi, Q_lo, Q_hi) ------
static void packSymbol(const Complex& sym, int8_t* dst) {
    int16_t I_val = (int16_t)round(sym.I * 32767.0);
    int16_t Q_val = (int16_t)round(sym.Q * 32767.0);
    uint8_t I_bytes[2], Q_bytes[2];
    memcpy(I_bytes, &I_val, 2);
    memcpy(Q_bytes, &Q_val, 2);
    dst[0] = (int8_t)((int16_t)I_bytes[0] - 128);
    dst[1] = (int8_t)((int16_t)I_bytes[1] - 128);
    dst[2] = (int8_t)((int16_t)Q_bytes[0] - 128);
    dst[3] = (int8_t)((int16_t)Q_bytes[1] - 128);
}

// ---- Build one 64-subcarrier OFDM symbol (256 bytes) --------
// 'dataBits'   : pointer to NCBPS bits in right-MSB order
// 'NBPSC'      : bits per subcarrier
// 'pilotIdx'   : pilot sequence position (0-based, mod 127)
static void buildOfdmSymbol(const uint8_t* dataBits, int NBPSC, int pilotIdx, int8_t* ofdmOut) {
    // Zero all 64 subcarriers
    Complex subcarriers[64] = {};

    // Map 48 data symbols
    for (int i = 0; i < 48; i++) {
        Complex sym = mapBitsToSymbol(dataBits + i * NBPSC, NBPSC);
        int sc_idx   = DATA_SUBCARRIERS[i];    // -26..+26 (no pilots/nulls)
        int arr_idx  = sc_idx + 32;            // 0-based index into 64-array (DC = index 32)
        subcarriers[arr_idx] = sym;
    }

    // Insert pilots: p_n = PILOT_SEQ[pilotIdx % 127] (same value for all 4 pilots)
    double p_n = (double)PILOT_SEQ[pilotIdx % 127];
    for (int i = 0; i < 4; i++) {
        int arr_idx = PILOT_SUBCARRIERS[i] + 32;
        subcarriers[arr_idx] = {p_n, 0.0};
    }

    // Pack into output buffer (64 × 4 bytes = 256 bytes)
    for (int i = 0; i < 64; i++) {
        packSymbol(subcarriers[i], ofdmOut + i * 4);
    }
}

// ---- State --------------------------------------------------
struct QamMapperData {
    int frameCount;
    int totalOfdmSymbols;
};

QamMapperData init_qam_mapper(const BlockConfig& config) {
    buildConstellations();

    QamMapperData data;
    data.frameCount       = 0;
    data.totalOfdmSymbols = 0;

    printf("[QamMapper] IEEE 802.11a QAM mapper with OFDM symbol structuring\n");
    printf("[QamMapper] Output: 256 bytes/symbol (64 subcarriers × 4 bytes)\n");
    printf("[QamMapper] SIGNAL: 1 symbol (BPSK, 48 bits from first 6 bytes of input)\n");
    printf("[QamMapper] DATA:   N_SYM symbols (rate-dependent modulation)\n");
    printf("[QamMapper] Pilot PN sequence: 127-element (cyclic)\n");
    printf("[QamMapper] Protocol:\n");
    printf("[QamMapper]   1. Read interleaved DATA (in[0]) -- FIRST\n");
    printf("[QamMapper]   2. Read rate_encoded_length (in[1])\n");
    printf("[QamMapper]   3. Send SIGNAL OFDM symbol (out[0])\n");
    printf("[QamMapper]   4. Send N_SYM DATA OFDM symbols (out[0])\n");
    printf("[QamMapper] Ready\n");

    return data;
}

void process_qam_mapper(
    const char** pipeIn,
    const char** pipeOut,
    QamMapperData& customData,
    const BlockConfig& config
) {
    // Pipeline wiring (argv order from blockUtils topological sort):
    //   argv[1] = in[0] = rate_encoded_length pipe (3 bytes/pkt)   -- P18
    //   argv[2] = in[1] = Interleaved DATA pipe    (3030 bytes/pkt) -- P17
    //   argv[3] = out[0] = OFDM symbols            (256 bytes/sym)  -- P21
    //
    // Each input frame produces (1 + N_SYM) OFDM symbols.
    // All OFDM symbols for the frame are collected into a single heap buffer
    // and sent with one write() call to avoid calling write() thousands of times.
    PipeIO inRate  (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);
    PipeIO inData  (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);
    PipeIO outOfdm (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);

    const int inRatePkt = config.inputPacketSizes[0];  // 3
    const int inDataPkt = config.inputPacketSizes[1];  // 3030

    int8_t* rateBuf = new int8_t[inRate.getBufferSize()];
    int8_t* dataBuf = new int8_t[inData.getBufferSize()];

    const bool isFirstBatch = (customData.frameCount == 0);

    // ===== STEP 1: Read rate_encoded_length batch -- FIRST (matches in[0]) =====
    int actualCount = inRate.read(rateBuf);

    // ===== STEP 2: Read interleaved DATA batch =====
    inData.read(dataBuf);

    // ===== STEP 3 + 4: Process each frame in the batch =====
    for (int pktIdx = 0; pktIdx < actualCount; pktIdx++) {
        const int dataOff = pktIdx * inDataPkt;
        const int rateOff = pktIdx * inRatePkt;

        // Extract rate and encoded length
        uint8_t rateVal   = (uint8_t)((int32_t)rateBuf[rateOff + 0] + 128);
        uint8_t encLenLo  = (uint8_t)((int32_t)rateBuf[rateOff + 1] + 128);
        uint8_t encLenHi  = (uint8_t)((int32_t)rateBuf[rateOff + 2] + 128);
        int encodedLength = (int)encLenLo | ((int)encLenHi << 8);
        if (encodedLength > inDataPkt) encodedLength = inDataPkt;

        // Convert meaningful bytes to uint8 and expand to bits (right-MSB)
        int numBits = encodedLength * 8;
        uint8_t* inputBits = new uint8_t[numBits]();
        for (int b = 0; b < encodedLength; b++) {
            uint8_t byteVal = (uint8_t)((int32_t)dataBuf[dataOff + b] + 128);
            for (int bit = 0; bit < 8; bit++) {
                inputBits[b * 8 + bit] = (byteVal >> bit) & 1;  // right-MSB
            }
        }

        // Get DATA rate params
        RateModParams mp = getRateModParams(rateVal);

        // N_SYM calculation (must match demapper)
        const int L_PPDU    = 16 + (1500 * 8) + 32 + 6;  // 12054 bits
        int N_SYM           = (L_PPDU + mp.NDBPS - 1) / mp.NDBPS;
        int totalBitsNeeded = N_SYM * mp.NCBPS;
        int totalSymbols    = 1 + N_SYM;  // 1 SIGNAL + N_SYM DATA

        if (isFirstBatch && pktIdx < 3) {
            printf("\n========================================\n");
            printf("[QamMapper] Frame %d\n", customData.frameCount + pktIdx + 1);
            printf("========================================\n");
            printf("  RATE value    : %u\n", (unsigned)rateVal);
            printf("  Encoded length: %d bytes (%d bits)\n", encodedLength, numBits);
            printf("  Modulation    : NBPSC=%d, NCBPS=%d, NDBPS=%d\n",
                   mp.NBPSC, mp.NCBPS, mp.NDBPS);
            printf("  N_SYM         : %d (+ 1 SIGNAL = %d total symbols)\n",
                   N_SYM, totalSymbols);
            printf("  Bits needed   : %d\n", totalBitsNeeded);
        }

        // Allocate output buffer for all OFDM symbols of this frame
        // (1 SIGNAL + N_SYM DATA) * 256 bytes each
        int8_t* frameBuf = new int8_t[totalSymbols * 256]();

        // ---- SIGNAL OFDM symbol (48 bits, always BPSK) ----
        uint8_t signalBits[48] = {};
        if (numBits >= 48) {
            memcpy(signalBits, inputBits, 48);
        } else {
            memcpy(signalBits, inputBits, numBits);
        }
        buildOfdmSymbol(signalBits, 1, 0, frameBuf + 0);  // pilotIdx=0 for SIGNAL

        if (isFirstBatch && pktIdx < 3) {
            printf("  Built SIGNAL OFDM symbol (BPSK)\n");
        }

        // ---- DATA OFDM symbols (N_SYM symbols) ----
        int availDataBits = numBits - 48;
        if (availDataBits < 0) availDataBits = 0;

        uint8_t* dataBitsAll = new uint8_t[totalBitsNeeded]();
        int copyLen = (availDataBits < totalBitsNeeded) ? availDataBits : totalBitsNeeded;
        if (copyLen > 0) {
            memcpy(dataBitsAll, inputBits + 48, copyLen);
        }

        for (int symIdx = 0; symIdx < N_SYM; symIdx++) {
            int startBit = symIdx * mp.NCBPS;
            int pilotIdx = symIdx + 1;  // SIGNAL uses 0, DATA starts at 1
            buildOfdmSymbol(dataBitsAll + startBit, mp.NBPSC, pilotIdx,
                            frameBuf + (1 + symIdx) * 256);
        }

        if (isFirstBatch && pktIdx < 3) {
            printf("  Built %d DATA OFDM symbols (%s)\n", N_SYM,
                   (mp.NBPSC == 1) ? "BPSK" :
                   (mp.NBPSC == 2) ? "QPSK" :
                   (mp.NBPSC == 4) ? "16QAM" : "64QAM");
            printf("  Sending %d OFDM symbols in one write call\n", totalSymbols);
        }

        // Single write of all OFDM symbols for this frame, one at a time
        // (PipeIO batchSize=1, so each write() call sends exactly 256 bytes)
        for (int s = 0; s < totalSymbols; s++) {
            outOfdm.write(frameBuf + s * 256, 1);
        }
        customData.totalOfdmSymbols += totalSymbols;

        delete[] inputBits;
        delete[] dataBitsAll;
        delete[] frameBuf;
    }

    customData.frameCount += actualCount;

    delete[] dataBuf;
    delete[] rateBuf;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr,
            "Usage: qam_mapper <pipeInRate> <pipeInData> <pipeOutOfdm>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1], argv[2]};  // [0]=rate(3B), [1]=data(3030B)
    const char* pipeOut    = argv[3];

    BlockConfig config = {
        "QamMapper",
        2,                       // inputs
        1,                       // outputs
        {3, 3030},               // inputPacketSizes  [rate_enc_len(argv[1]), INT_DATA(argv[2])]
        {6000, 6000},            // inputBatchSizes
        {256},                   // outputPacketSizes [OFDM_SYMBOL]
        {1},                     // outputBatchSizes  -- write 1 symbol at a time
        false,                    // ltr
        true,                    // startWithAll
        "IEEE 802.11a QAM mapper: rate(in[0]) + data(in[1]) -> OFDM symbols (SIGNAL + N_SYM DATA)"
    };

    run_manual_block(pipeIns, &pipeOut, config, process_qam_mapper, init_qam_mapper);
    return 0;
}
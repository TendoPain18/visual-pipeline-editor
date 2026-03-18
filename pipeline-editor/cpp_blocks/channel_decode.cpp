#include "core/run_generic_block.h"
#include <cstring>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <omp.h>

// ============================================================
// IEEE 802.11a Viterbi Channel Decoder
//
// Inputs:
//   in[0]: rate + DATA_ENCODED  (3025 bytes/pkt)  <- FIRST
//             Byte 0: rate_value
//             Bytes [1..3024]: encoded DATA bits packed, rest zero
//   in[1]: SIGNAL_ENCODED  (6 bytes/pkt = 48 encoded bits @ R=1/2)
//
// Outputs:
//   out[0]: Decoded DATA    (1515 bytes/pkt, max)  <- [0] rate counter
//   out[1]: Decoded SIGNAL  (3 bytes/pkt)          <- [1] -> descrambler in[1]
//
// Protocol (deadlock-free):
//   1. Read SIGNAL_ENC (in[0])  -- arrives first
//   2. Viterbi decode SIGNAL    [parallel over packets via OpenMP]
//   3. Send decoded SIGNAL (out[1]) -- ppdu_decap unblocks middleman
//   4. Read rate+DATA (in[1])   -- arrives after middleman gets feedback
//   5. Depuncture + Viterbi decode DATA  [parallel over packets via OpenMP]
//   6. Send decoded DATA (out[0])
//
// Performance:
//   - Trellis built once at init, shared read-only across all threads.
//   - Each OpenMP thread allocates its own ThreadBuffers on entry to the
//     parallel region -- private survivors, path metrics, and bit arrays.
//     Zero sharing between threads, no locks, no false sharing.
//   - Survivors stored as uint8_t (values 0-127).
// ============================================================

static const int NUM_STATES   = 64;
static const int ENC_BITS_MAX = 3024 * 8;          // 24192
static const int DEP_BITS_MAX = ENC_BITS_MAX * 2;  // 48384
static const int DEC_BITS_MAX = DEP_BITS_MAX / 2;  // 24192

static const int DEBUG_PKT_LIMIT = 3;

static const int G0_TAPS[7] = {1, 0, 1, 1, 0, 1, 1};
static const int G1_TAPS[7] = {1, 1, 1, 1, 0, 0, 1};

// -----------------------------------------------------------------------
// ACSEntry: forward table [prevState*2+inp] — used to build predTable.
// -----------------------------------------------------------------------
struct ACSEntry {
    uint8_t nextState;
    uint8_t outA;
    uint8_t outB;
    uint8_t survivorVal;
};

// -----------------------------------------------------------------------
// PredEntry: predecessor table for AVX2 ACS.
// Every state has exactly 2 predecessors in a K=7 rate-1/2 code.
// Layout: predTable[nextState * 2 + k],  k=0,1
// Full table: 64 * 2 * 4 bytes = 512 bytes — fits in L1.
// -----------------------------------------------------------------------
struct PredEntry {
    uint8_t pred;
    uint8_t outA;
    uint8_t outB;
    uint8_t survivorVal;  // = pred*2+inp
};

static ACSEntry  acsTable [NUM_STATES * 2];
static PredEntry predTable[NUM_STATES * 2];

static void build_trellis(ACSEntry* fwd, PredEntry* bwd) {
    int predCount[NUM_STATES] = {0};
    for (int state = 0; state < NUM_STATES; state++) {
        for (int inp = 0; inp <= 1; inp++) {
            int reg[7];
            reg[0] = inp;
            for (int b = 0; b < 6; b++) reg[b+1] = (state >> b) & 1;
            int outA = 0, outB = 0;
            for (int j = 0; j < 7; j++) {
                outA ^= (reg[j] & G0_TAPS[j]);
                outB ^= (reg[j] & G1_TAPS[j]);
            }
            int ns = 0;
            for (int b = 0; b < 6; b++) ns |= (reg[b] << b);

            ACSEntry& e   = fwd[state * 2 + inp];
            e.nextState   = (uint8_t)ns;
            e.outA        = (uint8_t)(outA & 1);
            e.outB        = (uint8_t)(outB & 1);
            e.survivorVal = (uint8_t)(state * 2 + inp);

            int k = predCount[ns]++;
            PredEntry& p  = bwd[ns * 2 + k];
            p.pred        = (uint8_t)state;
            p.outA        = (uint8_t)(outA & 1);
            p.outB        = (uint8_t)(outB & 1);
            p.survivorVal = (uint8_t)(state * 2 + inp);
        }
    }
}

// -----------------------------------------------------------------------
// AVX2 Viterbi decoder
//
// Iterates over nextState in groups of 8.
// For each group: gather predecessor metrics, add branch costs, min-reduce.
// Branch costs precomputed as a 4-entry lookup per time step.
// Survivors updated scalar (64 iters) after the SIMD metric pass.
//
// Compile with -mavx2 (included via -march=native).
// -----------------------------------------------------------------------
#include <immintrin.h>

static const int PM_INF = 0x7FFF;

static void viterbi_decode(
    const int* recvBits, int numPairs,
    int* decodedBits,
    const ACSEntry*  fwdTable,
    const PredEntry* bwdTable,
    uint8_t* survivorsBuf,
    int* pathMetric,
    int* newMetric
) {
    (void)fwdTable; (void)newMetric;
    if (numPairs <= 0) return;

    memset(survivorsBuf, 0, (size_t)NUM_STATES * (size_t)numPairs);

    // Aligned working metric arrays for AVX2 (256 bytes each)
    alignas(32) int pm[NUM_STATES];
    alignas(32) int nm[NUM_STATES];
    for (int s = 0; s < NUM_STATES; s++) pm[s] = PM_INF;
    pm[0] = 0;

    // Predecessor index arrays (built once, reused every time step)
    alignas(32) int pred0Idx[NUM_STATES];
    alignas(32) int pred1Idx[NUM_STATES];
    for (int ns = 0; ns < NUM_STATES; ns++) {
        pred0Idx[ns] = bwdTable[ns * 2 + 0].pred;
        pred1Idx[ns] = bwdTable[ns * 2 + 1].pred;
    }

    // Per-nextState branch cost arrays (filled each time step)
    alignas(32) int cost0Tab[NUM_STATES];
    alignas(32) int cost1Tab[NUM_STATES];

    for (int t = 0; t < numPairs; t++) {
        int recvA = recvBits[t * 2 + 0];
        int recvB = recvBits[t * 2 + 1];

        // Erasure masks
        int eraMaskA = (recvA >> 31) & 1;
        int eraMaskB = (recvB >> 31) & 1;

        // 4-entry branch cost lookup: bCost[outA*2 + outB]
        int bCost[4];
        for (int oA = 0; oA <= 1; oA++)
            for (int oB = 0; oB <= 1; oB++)
                bCost[oA*2+oB] = ((recvA ^ oA) & ~eraMaskA & 1)
                                + ((recvB ^ oB) & ~eraMaskB & 1);

        // Fill per-state cost tables
        for (int ns = 0; ns < NUM_STATES; ns++) {
            const PredEntry& p0 = bwdTable[ns * 2 + 0];
            const PredEntry& p1 = bwdTable[ns * 2 + 1];
            cost0Tab[ns] = bCost[p0.outA * 2 + p0.outB];
            cost1Tab[ns] = bCost[p1.outA * 2 + p1.outB];
        }

        // AVX2 ACS: 8 nextStates per iteration
        for (int ns = 0; ns < NUM_STATES; ns += 8) {
            __m256i vIdx0  = _mm256_load_si256((__m256i*)(pred0Idx + ns));
            __m256i vIdx1  = _mm256_load_si256((__m256i*)(pred1Idx + ns));
            __m256i vPm0   = _mm256_i32gather_epi32(pm, vIdx0, 4);
            __m256i vPm1   = _mm256_i32gather_epi32(pm, vIdx1, 4);
            __m256i vCost0 = _mm256_load_si256((__m256i*)(cost0Tab + ns));
            __m256i vCost1 = _mm256_load_si256((__m256i*)(cost1Tab + ns));
            __m256i vCand0 = _mm256_add_epi32(vPm0, vCost0);
            __m256i vCand1 = _mm256_add_epi32(vPm1, vCost1);
            __m256i vBest  = _mm256_min_epi32(vCand0, vCand1);
            _mm256_store_si256((__m256i*)(nm + ns), vBest);
        }

        // Survivors: scalar pass to record which predecessor won
        for (int ns = 0; ns < NUM_STATES; ns++) {
            const PredEntry& p0 = bwdTable[ns * 2 + 0];
            const PredEntry& p1 = bwdTable[ns * 2 + 1];
            int c0 = pm[p0.pred] + cost0Tab[ns];
            int c1 = pm[p1.pred] + cost1Tab[ns];
            survivorsBuf[ns * numPairs + t] =
                (c0 <= c1) ? p0.survivorVal : p1.survivorVal;
        }

        memcpy(pm, nm, NUM_STATES * sizeof(int));
    }

    int bestState = 0, bestMetric = pm[0];
    for (int s = 1; s < NUM_STATES; s++)
        if (pm[s] < bestMetric) { bestMetric = pm[s]; bestState = s; }

    int curState = bestState;
    for (int t = numPairs - 1; t >= 0; t--) {
        uint8_t sv     = survivorsBuf[curState * numPairs + t];
        decodedBits[t] = sv & 1;
        curState       = sv >> 1;
    }
}
// -----------------------------------------------------------------------
// Fused unpack + depuncture:  uint8_t bytes -> Viterbi int pairs directly.
//
// Replaces the old two-step: unpack_bytes_to_bits -> depuncture.
// Reads bits from packed bytes on the fly, writes only the pairs that
// survive the puncture pattern (erased slots written as -1).
// encBits[] array eliminated entirely — never materialised in memory.
//
// Returns numPairs (number of int pairs written to outPairs, where each
// pair is outPairs[t*2+0]=recvA, outPairs[t*2+1]=recvB).
// outPairs must have capacity >= dataFieldBytes * 8 * 2 ints (worst case R=1/2).
// -----------------------------------------------------------------------

// Helper: extract bit `bitIdx` from packed byte array (LSB-first)
static inline int get_bit(const uint8_t* buf, int bitIdx) {
    return (buf[bitIdx >> 3] >> (bitIdx & 7)) & 1;
}

static int depuncture_packed(
    const uint8_t* encBytes, int numEncBits,
    int* outPairs,           // output: interleaved recvA, recvB for Viterbi
    int rateCode
) {
    int outIdx = 0;   // indexes into outPairs (each pair = 2 ints)
    int inIdx  = 0;   // bit index into encBytes

    if (rateCode == 0) {
        // R=1/2: no puncturing — every input bit is valid, paired as A,B
        int numPairs = numEncBits / 2;
        for (int t = 0; t < numPairs; t++) {
            outPairs[t * 2 + 0] = get_bit(encBytes, inIdx++);
            outPairs[t * 2 + 1] = get_bit(encBytes, inIdx++);
        }
        return numPairs;
    }

    if (rateCode == 1) {
        // R=2/3: pattern cols [1,1 / 1,0]
        // col0: keep A (bit), keep B (bit)  -> 2 bits in, 2 out (A,B both valid)
        // col1: keep A (bit), erase B       -> 1 bit in, 2 out (A valid, B=-1)
        // Cycle = 2 input bits -> 3 encoded bits -> repeating col0,col1
        // onesPerGroup=3, pCols=2
        int numGroups = numEncBits / 3;
        for (int g = 0; g < numGroups; g++) {
            // col 0: rowA=1, rowB=1
            outPairs[outIdx++] = get_bit(encBytes, inIdx++);  // A valid
            outPairs[outIdx++] = get_bit(encBytes, inIdx++);  // B valid
            // col 1: rowA=1, rowB=0
            outPairs[outIdx++] = get_bit(encBytes, inIdx++);  // A valid
            outPairs[outIdx++] = -1;                           // B erased
        }
        return outIdx / 2;
    }

    // rateCode == 2: R=3/4: pattern cols [1,1,0 / 1,0,1]
    // col0: keep A, keep B  -> 2 bits in, 2 out
    // col1: keep A, erase B -> 1 bit in, 2 out (B=-1)
    // col2: erase A, keep B -> 1 bit in, 2 out (A=-1)
    // Cycle = 3 input bits -> 4 encoded bits
    int numGroups = numEncBits / 4;
    for (int g = 0; g < numGroups; g++) {
        // col 0: rowA=1, rowB=1
        outPairs[outIdx++] = get_bit(encBytes, inIdx++);  // A valid
        outPairs[outIdx++] = get_bit(encBytes, inIdx++);  // B valid
        // col 1: rowA=1, rowB=0
        outPairs[outIdx++] = get_bit(encBytes, inIdx++);  // A valid
        outPairs[outIdx++] = -1;                           // B erased
        // col 2: rowA=0, rowB=1
        outPairs[outIdx++] = -1;                           // A erased
        outPairs[outIdx++] = get_bit(encBytes, inIdx++);  // B valid
    }
    return outIdx / 2;
}

// Pack Viterbi decoded int bits directly to output bytes (LSB-first).
// Replaces pack_bits_to_bytes — no separate stack array needed.
static void pack_decbits_to_bytes(const int* bits, int numBits, uint8_t* outBytes, int maxBytes) {
    int nb = numBits / 8;
    if (nb > maxBytes) nb = maxBytes;
    memset(outBytes, 0, nb);
    for (int i = 0; i < nb * 8; i++)
        outBytes[i >> 3] |= (uint8_t)(bits[i] << (i & 7));
}

static int codingRateCode(uint8_t rv) {
    switch (rv) {
        case 13: return 0; // 1/2
        case 15: return 2; // 3/4
        case  5: return 0; // 1/2
        case  7: return 2; // 3/4
        case  9: return 0; // 1/2
        case 11: return 2; // 3/4
        case  1: return 1; // 2/3
        case  3: return 2; // 3/4
        default: return 0; // fallback to 1/2
    }
}
static const char* rateCodeName(int rc) {
    return rc==0?"1/2":rc==1?"2/3":"3/4";
}

// -----------------------------------------------------------------------
// Per-thread working buffers.
// Instantiated inside each OpenMP parallel region — one private copy per
// thread. The survivors buffer is heap-allocated (too large for stack);
// all other arrays fit on the stack inside this struct.
//
// Memory vs old layout (per thread):
//   OLD: encBits(97KB) + depBits(194KB) + decBits(97KB) = 388KB int arrays
//   NEW: depPairs(97KB) + decBits(97KB)                 = 194KB int arrays
//   Saving: ~194KB per thread (~1.2MB across 6 threads)
// -----------------------------------------------------------------------
struct ThreadBuffers {
    uint8_t* survivors;              // NUM_STATES * DEC_BITS_MAX bytes (heap)
    int      pathMetric[NUM_STATES]; // unused by AVX2 path but kept for API
    int      depPairs[DEC_BITS_MAX * 2 + 16];
    int      decBits[DEC_BITS_MAX + 16];
    uint8_t  decBytes[1515 + 4];
    uint8_t  encRaw[3024];

    ThreadBuffers()  { survivors = new uint8_t[(size_t)NUM_STATES * (size_t)DEC_BITS_MAX]; }
    ~ThreadBuffers() { delete[] survivors; }

    ThreadBuffers(const ThreadBuffers&) = delete;
    ThreadBuffers& operator=(const ThreadBuffers&) = delete;
};

// -----------------------------------------------------------------------
// Persistent block state (shared, read-only during parallel regions)
// -----------------------------------------------------------------------
struct ChannelDecodeData {
    ACSEntry  acsTable [NUM_STATES * 2];  // forward  table (512 bytes)
    PredEntry predTable[NUM_STATES * 2];  // backward table (512 bytes, AVX2 ACS)
    int frameCount;
    int errorFrames;
    int numThreads;
};

ChannelDecodeData init_channel_decode(const BlockConfig& config) {
    ChannelDecodeData data;
    data.frameCount  = 0;
    data.errorFrames = 0;
    build_trellis(data.acsTable, data.predTable);
    data.numThreads = omp_get_max_threads();

    printf("[ChannelDecode] ============================================\n");
    printf("[ChannelDecode] Viterbi Decoder initialized\n");
    printf("[ChannelDecode] g0=1011011, g1=1111001 (right-MSB), K=7, %d states\n", NUM_STATES);
    printf("[ChannelDecode] AVX2 ACS: predecessor table, 8-wide SIMD gather+min\n");
    printf("[ChannelDecode] ACS/pred tables: %zu bytes each (fits in L1)\n",
           sizeof(data.acsTable));
    printf("[ChannelDecode] ENC_BITS_MAX=%d  DEP_BITS_MAX=%d  DEC_BITS_MAX=%d\n",
           ENC_BITS_MAX, DEP_BITS_MAX, DEC_BITS_MAX);
    printf("[ChannelDecode] OpenMP parallel decode: %d threads\n", data.numThreads);
    printf("[ChannelDecode] Per-thread survivors: %zu bytes (uint8_t)\n",
           (size_t)NUM_STATES * (size_t)DEC_BITS_MAX);
    printf("[ChannelDecode] in[0]: rate+lip+DATA 3027B  in[1]: SIGNAL_ENC 6B\n");
    printf("[ChannelDecode] out[0]: lip+DATA 1517B     out[1]: SIGNAL 3B\n");
    printf("[ChannelDecode] enc_len lip from in[0] header bounds DATA Viterbi decode\n");
    printf("[ChannelDecode] frameBytes lip from decoded SIGNAL -> prepended to out[0]\n");
    printf("[ChannelDecode] Debug: first %d packets of first batch logged\n", DEBUG_PKT_LIMIT);
    printf("[ChannelDecode] ============================================\n");
    printf("[ChannelDecode] Ready\n");
    return data;
}

void process_channel_decode(
    const char** pipeIn,
    const char** pipeOut,
    ChannelDecodeData& customData,
    const BlockConfig& config
) {
    const int inDataPkt  = config.inputPacketSizes[0];   // 3027 [rate(1)|lip_lo(1)|lip_hi(1)|DATA(3024)]
    const int inSigPkt   = config.inputPacketSizes[1];   // 6
    const int outDataPkt = config.outputPacketSizes[0];  // 1517 [lip_lo(1)|lip_hi(1)|DATA(1515)]
    const int outSigPkt  = config.outputPacketSizes[1];  // 3

    PipeIO inData   (pipeIn[0],  inDataPkt,  config.inputBatchSizes[0]);
    PipeIO inSignal (pipeIn[1],  inSigPkt,   config.inputBatchSizes[1]);
    PipeIO outData  (pipeOut[0], outDataPkt, config.outputBatchSizes[0]);
    PipeIO outSignal(pipeOut[1], outSigPkt,  config.outputBatchSizes[1]);

    int8_t* sigInBuf   = new int8_t[inSignal.getBufferSize()];
    int8_t* dataInBuf  = new int8_t[inData.getBufferSize()];
    int8_t* sigOutBuf  = new int8_t[outSignal.getBufferSize()];
    int8_t* dataOutBuf = new int8_t[outData.getBufferSize()];

    memset(sigOutBuf,  0, outSignal.getBufferSize());
    memset(dataOutBuf, 0, outData.getBufferSize());

    const bool isFirstBatch = (customData.frameCount == 0);

    // ===== STEP 1: Read SIGNAL_ENCODED -- arrives first =====
    int actualCount = inSignal.read(sigInBuf);

    if (isFirstBatch) {
        printf("[ChannelDecode] First batch: actualCount=%d\n", actualCount);
    }

    // lipArr[i]    = frameBytes lip (from decoded SIGNAL) -> written to out[0] header
    // encLipArr[i] = encoded DATA lip from in[0] header  -> bounds DATA Viterbi decode
    // errorArr[i]  = overflow flag
    int* lipArr    = new int[actualCount + 1];
    int* encLipArr = new int[actualCount + 1];
    int* errorArr  = new int[actualCount]();  // zero-init

    for (int i = 0; i <= actualCount; i++) {
        lipArr[i]    = 1515;
        encLipArr[i] = inDataPkt - 3; // 3024 safe default
    }

    // ===== STEP 2: Decode SIGNAL -- parallel over packets =====
    #pragma omp parallel num_threads(customData.numThreads)
    {
        ThreadBuffers tb;

        #pragma omp for schedule(dynamic, 1)
        for (int i = 0; i < actualCount; i++) {
            const bool dbg      = isFirstBatch && (i < DEBUG_PKT_LIMIT);
            const int  sigOff    = i * inSigPkt;
            const int  sigOutOff = i * outSigPkt;

            uint8_t sigBytes[6];
            for (int j = 0; j < 6; j++)
                sigBytes[j] = (uint8_t)((int32_t)sigInBuf[sigOff + j] + 128);

            if (dbg) {
                printf("[ChannelDecode] DBG pkt[%d] SIGNAL_ENC: "
                       "%02X %02X %02X %02X %02X %02X\n",
                       i, sigBytes[0], sigBytes[1], sigBytes[2],
                          sigBytes[3], sigBytes[4], sigBytes[5]);
            }

            // Unpack 6 signal bytes -> 24 pairs (R=1/2, no erasures) directly
            int sigPairs[48];  // 24 pairs * 2 ints
            for (int j = 0; j < 48; j++)
                sigPairs[j] = get_bit(sigBytes, j);

            int sigDecBits[24] = {0};
            viterbi_decode(sigPairs, 24, sigDecBits, customData.acsTable,
                           customData.predTable,
                           tb.survivors, nullptr, nullptr);

            uint8_t decSig[3] = {0,0,0};
            pack_decbits_to_bytes(sigDecBits, 24, decSig, 3);

            if (dbg) {
                printf("[ChannelDecode] DBG pkt[%d] SIGNAL decoded: %02X %02X %02X\n",
                       i, decSig[0], decSig[1], decSig[2]);
            }

            uint8_t  rateVal = decSig[0] & 0x0F;
            uint16_t length  = (uint16_t)((decSig[0] >> 5) & 0x07)
                             | ((uint16_t)decSig[1] << 3)
                             | (((uint16_t)(decSig[2] & 0x01)) << 11);

            if (dbg) {
                printf("[ChannelDecode] DBG pkt[%d] SIGNAL: rateVal=%u macLen=%u\n",
                       i, (unsigned)rateVal, (unsigned)length);
            }

            if (length == 0 || length > 4095) {
                printf("[ChannelDecode] pkt[%d] WARNING: bad mac_length=%u, clamping\n",
                       i, (unsigned)length);
                length = 1504;
            }

            int ndbps = 48;
            switch (rateVal) {
                case 13: ndbps =  24; break;
                case 15: ndbps =  36; break;
                case  5: ndbps =  48; break;
                case  7: ndbps =  72; break;
                case  9: ndbps =  96; break;
                case 11: ndbps = 144; break;
                case  1: ndbps = 192; break;
                case  3: ndbps = 216; break;
                default:
                    printf("[ChannelDecode] pkt[%d] WARNING: unknown rateVal=%u\n",
                           i, (unsigned)rateVal);
            }

            int dbits   = 16 + (int)length * 8 + 6;
            int nsym    = (dbits + ndbps - 1) / ndbps;
            int padbits = nsym * ndbps - dbits;
            int tpbytes = (6 + padbits + 7) / 8;
            int lip     = 3 + 2 + (int)length + tpbytes;

            if (dbg) {
                printf("[ChannelDecode] DBG pkt[%d] ndbps=%d dbits=%d nsym=%d "
                       "padbits=%d tpbytes=%d lip=%d\n",
                       i, ndbps, dbits, nsym, padbits, tpbytes, lip);
            }

            if (lip < 5)          lip = 5;
            if (lip > 1515)       lip = 1515;
            lipArr[i] = lip;   // i unique per thread -- no race

            for (int j = 0; j < 3; j++)
                sigOutBuf[sigOutOff + j] = (int8_t)((int32_t)decSig[j] - 128);
        }
    } // implicit barrier: all SIGNAL decoded before we send

    // ===== STEP 3: Send SIGNAL (out[1]) =====
    outSignal.write(sigOutBuf, actualCount);

    // ===== STEP 4: Read rate+lip+DATA (in[0]) =====
    // in[0] layout: [rate(1)|lip_lo(1)|lip_hi(1)|DATA_DEINT(3024)]
    // Extract enc_len lip from each packet header and store in encLipArr.
    inData.read(dataInBuf);

    // Parse enc_len from each packet header (single-threaded, fast)
    for (int i = 0; i < actualCount; i++) {
        const int dataInOff = i * inDataPkt;
        // byte 0 = rate (also read per-packet below in parallel)
        uint8_t encLipLo = (uint8_t)((int32_t)dataInBuf[dataInOff + 1] + 128);
        uint8_t encLipHi = (uint8_t)((int32_t)dataInBuf[dataInOff + 2] + 128);
        int encLip = (int)encLipLo | ((int)encLipHi << 8);
        if (encLip < 0)              encLip = 0;
        if (encLip > inDataPkt - 3) encLip = inDataPkt - 3; // cap at 3024
        encLipArr[i] = encLip;
    }

    // ===== STEP 5: Decode DATA -- parallel over packets =====
    #pragma omp parallel num_threads(customData.numThreads)
    {
        ThreadBuffers tb;

        #pragma omp for schedule(dynamic, 1)
        for (int i = 0; i < actualCount; i++) {
            const bool dbg       = isFirstBatch && (i < DEBUG_PKT_LIMIT);
            const int  dataInOff  = i * inDataPkt;
            const int  dataOutOff = i * outDataPkt;

            uint8_t rateVal  = (uint8_t)((int32_t)dataInBuf[dataInOff] + 128);
            int     rateCode = codingRateCode(rateVal);

            if (dbg) {
                printf("[ChannelDecode] DBG pkt[%d] DATA: rateVal=%u R=%s\n",
                       i, (unsigned)rateVal, rateCodeName(rateCode));
            }

            int frameLip   = lipArr[i];    // PPDU frame bytes (from decoded SIGNAL)
            int encLip     = encLipArr[i]; // Encoded DATA bytes (from in[0] header)

            // encLip is the number of meaningful encoded DATA bytes.
            // We compute numEncBits directly from encLip (no need to derive from SIGNAL).
            // Each coding rate produces bits at a fixed ratio; encLip bytes = encLip*8 bits.
            // We use all of them.
            int numEncBits = encLip * 8;
            int encDataBytes = encLip;
            if (encDataBytes > inDataPkt - 3) encDataBytes = inDataPkt - 3;

            if (dbg) {
                printf("[ChannelDecode] DBG pkt[%d] frameLip=%d encLip=%d "
                       "encDataBytes=%d numEncBits=%d\n",
                       i, frameLip, encLip, encDataBytes, numEncBits);
            }

            // DATA bytes start at offset 3 in in[0] (skip rate+lip_lo+lip_hi)
            for (int j = 0; j < encDataBytes; j++)
                tb.encRaw[j] = (uint8_t)((int32_t)dataInBuf[dataInOff + 3 + j] + 128);

            if (dbg) {
                printf("[ChannelDecode] DBG pkt[%d] DATA_ENC first bytes: "
                       "%02X %02X %02X %02X %02X %02X %02X %02X\n",
                       i, tb.encRaw[0], tb.encRaw[1], tb.encRaw[2], tb.encRaw[3],
                          tb.encRaw[4], tb.encRaw[5], tb.encRaw[6], tb.encRaw[7]);
            }

            // Fused unpack+depuncture: bytes -> Viterbi int pairs in one pass
            int numPairs = depuncture_packed(tb.encRaw, numEncBits, tb.depPairs, rateCode);

            if (numPairs > DEC_BITS_MAX) {
                printf("[ChannelDecode] CRITICAL pkt[%d]: overflow! "
                       "numPairs=%d DEC_MAX=%d -- CLAMPING\n",
                       i, numPairs, DEC_BITS_MAX);
                numPairs = DEC_BITS_MAX;
                errorArr[i] = 1;
            }

            memset(tb.decBits, 0, numPairs * sizeof(int));
            viterbi_decode(tb.depPairs, numPairs, tb.decBits,
                           customData.acsTable, customData.predTable,
                           tb.survivors, nullptr, nullptr);

            int dataFieldBytes = frameLip - 3;  // exclude SIGNAL 3B
            if (dataFieldBytes < 0) dataFieldBytes = 0;
            int numDecBytes = frameLip;
            if (numDecBytes > outDataPkt - 2) numDecBytes = outDataPkt - 2;

            memset(tb.decBytes, 0, numDecBytes);

            // bytes[0..2]: decoded SIGNAL (from sigOutBuf)
            {
                const int sigOutOff2 = i * outSigPkt;
                for (int j = 0; j < 3 && j < numDecBytes; j++)
                    tb.decBytes[j] = (uint8_t)((int32_t)sigOutBuf[sigOutOff2 + j] + 128);
            }
            // bytes[3..frameLip-1]: Viterbi decoded data field
            {
                int dfc = dataFieldBytes;
                if (dfc > outDataPkt - 2 - 3) dfc = outDataPkt - 2 - 3;
                pack_decbits_to_bytes(tb.decBits, dfc * 8, tb.decBytes + 3, dfc);
            }

            if (dbg) {
                printf("[ChannelDecode] DBG pkt[%d] first decoded bytes: "
                       "%02X %02X %02X %02X %02X %02X %02X %02X\n",
                       i,
                       tb.decBytes[0], tb.decBytes[1], tb.decBytes[2], tb.decBytes[3],
                       tb.decBytes[4], tb.decBytes[5], tb.decBytes[6], tb.decBytes[7]);
            }

            // out[0] layout: [lip_lo(1) | lip_hi(1) | DATA(1515)]
            // Write lip header first (frameLip = PPDU frame bytes)
            dataOutBuf[dataOutOff + 0] = (int8_t)((int32_t)(frameLip & 0xFF)        - 128);
            dataOutBuf[dataOutOff + 1] = (int8_t)((int32_t)((frameLip >> 8) & 0xFF) - 128);
            // Then write decoded PPDU bytes starting at offset 2
            for (int j = 0; j < numDecBytes; j++)
                dataOutBuf[dataOutOff + 2 + j] = (int8_t)((int32_t)tb.decBytes[j] - 128);
        }
    } // implicit barrier

    // ===== STEP 6: Send DATA (out[0]) =====
    outData.write(dataOutBuf, actualCount);

    int batchErrors = 0;
    for (int i = 0; i < actualCount; i++) batchErrors += errorArr[i];
    customData.errorFrames += batchErrors;
    customData.frameCount  += actualCount;

    printf("[ChannelDecode] batch done: totalFrames=%d errorFrames=%d\n",
           customData.frameCount, customData.errorFrames);

    delete[] lipArr;
    delete[] encLipArr;
    delete[] errorArr;
    delete[] sigInBuf;
    delete[] dataInBuf;
    delete[] sigOutBuf;
    delete[] dataOutBuf;
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr,
            "Usage: channel_decode <pipeInData> <pipeInSignal>"
            " <pipeOutData> <pipeOutSignal>\n");
        return 1;
    }
    printf("[ChannelDecode] args: in[0]=%s in[1]=%s out[0]=%s out[1]=%s\n",
           argv[1], argv[2], argv[3], argv[4]);

    const char* pipeIns[]  = {argv[1], argv[2]};
    const char* pipeOuts[] = {argv[3], argv[4]};

    BlockConfig config = {
        "ChannelDecode",
        2,               // inputs
        2,               // outputs
        {3027, 6},       // inputPacketSizes  [rate+lip+DATA_DEINT(3027), SIGNAL_ENC(6)]
        {6000, 6000},    // inputBatchSizes
        {1517, 3},       // outputPacketSizes [lip+DATA(1517), SIGNAL(3)]
        {6000, 6000},    // outputBatchSizes
        true,            // ltr
        true,            // startWithAll
        "Viterbi decoder: SIGNAL decoded+sent first; DATA decoded with enc_len lip from header; out[0]=lip+DATA"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_channel_decode, init_channel_decode);
    return 0;
}
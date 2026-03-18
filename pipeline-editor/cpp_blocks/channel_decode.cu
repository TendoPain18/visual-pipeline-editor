#include "core/run_generic_block.h"
#include <cstring>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <memory>
#include <cuda_runtime.h>
#include <algorithm>
#include <omp.h>

// ============================================================
// IEEE 802.11a Viterbi Channel Decoder — GPU-accelerated
//
// SIGNAL decode: CPU (only 24 pairs, GPU dispatch overhead > compute cost)
// DATA decode:   CUDA GPU — one thread block per packet, 64 threads per
//                block (one per Viterbi state), shared memory for metrics.
//
// Optimizations applied:
//   1. Async CUDA stream (H2D + kernel + D2H non-blocking)
//   2. OpenMP parallel depuncture across all packets simultaneously
//   3. Bit-packed survivors (1 bit per step vs 8 bits — 8x bandwidth reduction)
//   4. Warp shuffle best-state reduction (replaces serial thread-0 scan)
//   5. Double-buffered pm[] — 1 __syncthreads() per step instead of 2
//   6. Compact H2D/D2H transfer — only live bytes, ~75% less PCIe bandwidth
//   7. Persistent pinned IO buffers — no heap alloc/free per batch
//   8. Parallel output packing (Step 7) via OpenMP
//   9. Persistent per-batch scratch arrays (lipArr, encLipArr, errorArr)
//      moved into GpuBufs — eliminates new/delete on every batch call
//  10. Single-pass depuncture — depuncture_to_int8() writes int8_t directly
//      into a per-PACKET staging slot (h_stagingBuf[i]); Pass 2 is a parallel
//      memcpy staging → compacted h_pairs. No aliasing, no second depuncture,
//      no pack_pairs_to_int8(). Replaces buggy per-thread scratch approach.
// ============================================================

static const int NUM_STATES   = 64;
static const int ENC_BITS_MAX = 3024 * 8;
static const int DEP_BITS_MAX = ENC_BITS_MAX * 2;
static const int DEC_BITS_MAX = DEP_BITS_MAX / 2;

static const int G0_TAPS[7] = {1, 0, 1, 1, 0, 1, 1};
static const int G1_TAPS[7] = {1, 1, 1, 1, 0, 0, 1};

// -----------------------------------------------------------------------
// Trellis tables
// -----------------------------------------------------------------------
struct ACSEntry {
    uint8_t nextState;
    uint8_t outA;
    uint8_t outB;
    uint8_t survivorVal;
};

struct PredEntry {
    uint8_t pred;
    uint8_t outA;
    uint8_t outB;
    uint8_t survivorVal;
};

// GPU constant memory — broadcast-cached, zero-cost multi-read
__constant__ uint8_t d_pred0  [NUM_STATES];
__constant__ uint8_t d_pred1  [NUM_STATES];
__constant__ uint8_t d_p0outA [NUM_STATES];
__constant__ uint8_t d_p0outB [NUM_STATES];
__constant__ uint8_t d_p1outA [NUM_STATES];
__constant__ uint8_t d_p1outB [NUM_STATES];
__constant__ uint8_t d_p0surv [NUM_STATES];
__constant__ uint8_t d_p1surv [NUM_STATES];

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

static void upload_trellis_to_gpu(const PredEntry* bwd) {
    uint8_t h_pred0[NUM_STATES], h_pred1[NUM_STATES];
    uint8_t h_p0outA[NUM_STATES], h_p0outB[NUM_STATES];
    uint8_t h_p1outA[NUM_STATES], h_p1outB[NUM_STATES];
    uint8_t h_p0surv[NUM_STATES], h_p1surv[NUM_STATES];

    for (int ns = 0; ns < NUM_STATES; ns++) {
        h_pred0[ns]  = bwd[ns * 2 + 0].pred;
        h_pred1[ns]  = bwd[ns * 2 + 1].pred;
        h_p0outA[ns] = bwd[ns * 2 + 0].outA;
        h_p0outB[ns] = bwd[ns * 2 + 0].outB;
        h_p1outA[ns] = bwd[ns * 2 + 1].outA;
        h_p1outB[ns] = bwd[ns * 2 + 1].outB;
        h_p0surv[ns] = bwd[ns * 2 + 0].survivorVal;
        h_p1surv[ns] = bwd[ns * 2 + 1].survivorVal;
    }

    cudaMemcpyToSymbol(d_pred0,  h_pred0,  NUM_STATES);
    cudaMemcpyToSymbol(d_pred1,  h_pred1,  NUM_STATES);
    cudaMemcpyToSymbol(d_p0outA, h_p0outA, NUM_STATES);
    cudaMemcpyToSymbol(d_p0outB, h_p0outB, NUM_STATES);
    cudaMemcpyToSymbol(d_p1outA, h_p1outA, NUM_STATES);
    cudaMemcpyToSymbol(d_p1outB, h_p1outB, NUM_STATES);
    cudaMemcpyToSymbol(d_p0surv, h_p0surv, NUM_STATES);
    cudaMemcpyToSymbol(d_p1surv, h_p1surv, NUM_STATES);
}

// -----------------------------------------------------------------------
// CPU Viterbi — SIGNAL only (24 pairs, not worth GPU dispatch)
// -----------------------------------------------------------------------
static const int PM_INF = 0x7FFF;

static void cpu_viterbi_decode(
    const int* recvBits, int numPairs,
    int* decodedBits,
    const PredEntry* bwdTable,
    uint8_t* survivorsBuf
) {
    if (numPairs <= 0) return;
    memset(survivorsBuf, 0, (size_t)NUM_STATES * (size_t)numPairs);

    int pm[NUM_STATES], nm[NUM_STATES];
    for (int s = 0; s < NUM_STATES; s++) pm[s] = PM_INF;
    pm[0] = 0;

    for (int t = 0; t < numPairs; t++) {
        int recvA = recvBits[t * 2 + 0];
        int recvB = recvBits[t * 2 + 1];
        int eraMaskA = (recvA >> 31) & 1;
        int eraMaskB = (recvB >> 31) & 1;

        int bCost[4];
        for (int oA = 0; oA <= 1; oA++)
            for (int oB = 0; oB <= 1; oB++)
                bCost[oA*2+oB] = ((recvA ^ oA) & ~eraMaskA & 1)
                                + ((recvB ^ oB) & ~eraMaskB & 1);

        for (int ns = 0; ns < NUM_STATES; ns++) {
            const PredEntry& p0 = bwdTable[ns * 2 + 0];
            const PredEntry& p1 = bwdTable[ns * 2 + 1];
            int c0 = pm[p0.pred] + bCost[p0.outA * 2 + p0.outB];
            int c1 = pm[p1.pred] + bCost[p1.outA * 2 + p1.outB];
            nm[ns] = (c0 <= c1) ? c0 : c1;
            survivorsBuf[ns * numPairs + t] = (c0 <= c1) ? p0.survivorVal : p1.survivorVal;
        }
        memcpy(pm, nm, NUM_STATES * sizeof(int));
    }

    int bestState = 0;
    for (int s = 1; s < NUM_STATES; s++)
        if (pm[s] < pm[bestState]) bestState = s;

    int curState = bestState;
    for (int t = numPairs - 1; t >= 0; t--) {
        uint8_t sv     = survivorsBuf[curState * numPairs + t];
        decodedBits[t] = sv & 1;
        curState       = sv >> 1;
    }
}

// -----------------------------------------------------------------------
// CUDA Viterbi kernel — bit-packed survivors + warp-shuffle best-state
//   Forward pass: double-buffered pm[], 1 __syncthreads() per step
//   Traceback: thread 0 serial
// -----------------------------------------------------------------------

__global__ void viterbi_kernel(
    const int8_t* __restrict__ d_pairs,
    const int*    __restrict__ d_numPairs,
    const int*    __restrict__ d_pairOffsets,
    uint32_t*                  d_survivors,   // [pkt * NUM_STATES * wordsPerPkt] bit-packed
    int8_t*                    d_decBits,
    const int*    __restrict__ d_decOffsets,
    int maxPairs,
    int wordsPerPkt
) {
    const int pkt = blockIdx.x;
    const int ns  = threadIdx.x;

    const int numPairs = d_numPairs[pkt];
    if (numPairs <= 0) return;

    const int8_t* pairs    = d_pairs    + d_pairOffsets[pkt];
    uint32_t*     survBase = d_survivors + (size_t)pkt * NUM_STATES * wordsPerPkt;
    int8_t*       decBits  = d_decBits  + d_decOffsets[pkt];

    __shared__ int pm[2][NUM_STATES];
    int cur = 0, nxt = 1;
    pm[cur][ns] = (ns == 0) ? 0 : PM_INF;
    __syncthreads();

    // Pre-clear survivors
    for (int w = 0; w < wordsPerPkt; w++)
        survBase[ns * wordsPerPkt + w] = 0u;
    __syncthreads();

    // ---- Forward pass ----
    for (int t = 0; t < numPairs; t++) {
        int recvA    = (int)pairs[t * 2 + 0];
        int recvB    = (int)pairs[t * 2 + 1];
        int eraMaskA = (recvA < 0) ? 1 : 0;
        int eraMaskB = (recvB < 0) ? 1 : 0;

        int m0 = pm[cur][d_pred0[ns]];
        int m1 = pm[cur][d_pred1[ns]];

        int cost0 = ((recvA ^ (int)d_p0outA[ns]) & ~eraMaskA & 1)
                  + ((recvB ^ (int)d_p0outB[ns]) & ~eraMaskB & 1);
        int cost1 = ((recvA ^ (int)d_p1outA[ns]) & ~eraMaskA & 1)
                  + ((recvB ^ (int)d_p1outB[ns]) & ~eraMaskB & 1);

        int c0 = (m0 >= PM_INF) ? PM_INF : m0 + cost0;
        int c1 = (m1 >= PM_INF) ? PM_INF : m1 + cost1;

        bool pick0 = (c0 <= c1);
        pm[nxt][ns] = pick0 ? c0 : c1;

        // Each thread writes to its own row — no conflicts, no atomics needed
        int wordIdx = t >> 5;
        int bitIdx  = t & 31;
        if (!pick0) survBase[ns * wordsPerPkt + wordIdx] |= (1u << bitIdx);

        __syncthreads();
        cur ^= 1; nxt ^= 1;
    }

    // ---- Best-state: warp shuffle reduction ----
    __shared__ int warpMinMetric[2];
    __shared__ int warpMinState [2];

    int myMetric = pm[cur][ns];
    int myState  = ns;
    int lane     = ns & 31;
    int warpId   = ns >> 5;

    for (int offset = 16; offset > 0; offset >>= 1) {
        int om = __shfl_down_sync(0xFFFFFFFF, myMetric, offset);
        int os = __shfl_down_sync(0xFFFFFFFF, myState,  offset);
        if (om < myMetric) { myMetric = om; myState = os; }
    }
    if (lane == 0) {
        warpMinMetric[warpId] = myMetric;
        warpMinState [warpId] = myState;
    }
    __syncthreads();

    if (ns == 0) {
        int bestState = (warpMinMetric[0] <= warpMinMetric[1])
                        ? warpMinState[0] : warpMinState[1];

        int curState = bestState;
        for (int t = numPairs - 1; t >= 0; t--) {
            int      wordIdx = t >> 5;
            int      bitIdx  = t & 31;
            uint32_t word    = survBase[curState * wordsPerPkt + wordIdx];
            int      pick1   = (word >> bitIdx) & 1;

            decBits[t] = pick1 ? (int8_t)(d_p1surv[curState] & 1)
                                : (int8_t)(d_p0surv[curState] & 1);

            curState = pick1 ? (int)d_pred1[curState] : (int)d_pred0[curState];
        }
    }
}

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------
static inline int get_bit(const uint8_t* buf, int bitIdx) {
    return (buf[bitIdx >> 3] >> (bitIdx & 7)) & 1;
}

// Single-pass depuncture — writes int8_t directly, no intermediate int* buffer.
// Erased symbols are stored as -1 (same sentinel the kernel checks for erasure).
// Returns number of pairs produced.
static int depuncture_to_int8(
    const uint8_t* encBytes, int numEncBits,
    int8_t* outPairs, int rateCode
) {
    int outIdx = 0, inIdx = 0;
    if (rateCode == 0) {
        int numPairs = numEncBits / 2;
        for (int t = 0; t < numPairs; t++) {
            outPairs[t * 2 + 0] = (int8_t)get_bit(encBytes, inIdx++);
            outPairs[t * 2 + 1] = (int8_t)get_bit(encBytes, inIdx++);
        }
        return numPairs;
    }
    if (rateCode == 1) {
        int numGroups = numEncBits / 3;
        for (int g = 0; g < numGroups; g++) {
            outPairs[outIdx++] = (int8_t)get_bit(encBytes, inIdx++);
            outPairs[outIdx++] = (int8_t)get_bit(encBytes, inIdx++);
            outPairs[outIdx++] = (int8_t)get_bit(encBytes, inIdx++);
            outPairs[outIdx++] = (int8_t)-1;
        }
        return outIdx / 2;
    }
    // rateCode == 2
    int numGroups = numEncBits / 4;
    for (int g = 0; g < numGroups; g++) {
        outPairs[outIdx++] = (int8_t)get_bit(encBytes, inIdx++);
        outPairs[outIdx++] = (int8_t)get_bit(encBytes, inIdx++);
        outPairs[outIdx++] = (int8_t)get_bit(encBytes, inIdx++);
        outPairs[outIdx++] = (int8_t)-1;
        outPairs[outIdx++] = (int8_t)-1;
        outPairs[outIdx++] = (int8_t)get_bit(encBytes, inIdx++);
    }
    return outIdx / 2;
}

static void pack_decbits_to_bytes(const int8_t* bits, int numBits, uint8_t* outBytes, int maxBytes) {
    int nb = numBits / 8;
    if (nb > maxBytes) nb = maxBytes;
    memset(outBytes, 0, nb);
    for (int i = 0; i < nb * 8; i++)
        outBytes[i >> 3] |= (uint8_t)(bits[i] << (i & 7));
}

static int codingRateCode(uint8_t rv) {
    switch (rv) {
        case 13: return 0; case 15: return 2;
        case  5: return 0; case  7: return 2;
        case  9: return 0; case 11: return 2;
        case  1: return 1; case  3: return 2;
        default: return 0;
    }
}

// -----------------------------------------------------------------------
// CPU buffers for SIGNAL decode
// -----------------------------------------------------------------------
struct SignalBuf {
    uint8_t survivors[NUM_STATES * 48 + 4];
    int     sigPairs[96];
    int     sigDecBits[48];
};

// -----------------------------------------------------------------------
// GPU batch buffers
// -----------------------------------------------------------------------
struct GpuBufs {
    // Host pinned
    int8_t*  h_pairs;        // compacted — packets packed contiguously, no padding
    int*     h_numPairs;
    int*     h_pairOffsets;  // h_pairOffsets[i] = start index in h_pairs for packet i
    int8_t*  h_decBits;      // compacted output — same layout as h_pairs
    int*     h_decOffsets;   // h_decOffsets[i] = start index in h_decBits for packet i

    // Per-packet staging buffer for single-pass depuncture.
    // h_stagingBuf[i] points to slot i — each packet depunctures directly into
    // its own slot, so no thread aliasing regardless of OMP scheduling.
    // Layout: flat array of [maxBatch * maxPairs * 2] int8_t, sliced per packet.
    int8_t*  h_stagingBuf;      // flat backing store
    int8_t** h_stagingSlot;     // [maxBatch] pointers into h_stagingBuf
    int      numOmpThreads;

    // Persistent IO buffers — allocated once, reused every batch
    int8_t*  sigInBuf;
    int8_t*  dataInBuf;
    int8_t*  sigOutBuf;
    int8_t*  dataOutBuf;
    int      sigInSize, dataInSize, sigOutSize, dataOutSize;

    // ---------------------------------------------------------------
    // Persistent per-batch scratch — replaces new/delete every call
    // These are plain host (not pinned) since they're CPU-only scratch.
    // ---------------------------------------------------------------
    int*  lipArr;       // [maxBatch]
    int*  encLipArr;    // [maxBatch]
    int*  errorArr;     // [maxBatch]

    // Device
    int8_t*  d_pairs;        // compacted on device
    int*     d_numPairs;
    int*     d_pairOffsets;
    uint32_t* d_survivors;
    int8_t*  d_decBits;      // compacted on device
    int*     d_decOffsets;

    int maxBatch;
    int maxPairs;
    int wordsPerPkt;
    cudaStream_t stream;

    // Persistent CUDA timing events — created once in alloc(), destroyed in
    // free_all(). Eliminates 4x cudaEventCreate + 4x cudaEventDestroy per batch.
    cudaEvent_t evH2Dstart, evH2Ddone, evKdone, evD2Hdone;

    // Timing accumulators (moved out of process function statics)
    int   timerBatch;
    float tH2D, tKernel, tD2H;
    LARGE_INTEGER tFreq, tBatchStart;

    void alloc(int batchSize, int maxP, int nThreads,
               int sigInSz, int dataInSz, int sigOutSz, int dataOutSz) {
        maxBatch      = batchSize;
        maxPairs      = maxP;
        wordsPerPkt   = (maxP + 31) / 32;
        numOmpThreads = nThreads;

        size_t pairsBytes    = (size_t)batchSize * maxP * 2 * sizeof(int8_t);
        size_t offsetBytes   = (size_t)(batchSize + 1) * sizeof(int);
        size_t numPairBytes  = (size_t)batchSize * sizeof(int);
        // Bit-packed survivors: [pkt * NUM_STATES * wordsPerPkt]
        size_t survBytes     = (size_t)batchSize * NUM_STATES * wordsPerPkt * sizeof(uint32_t);

        cudaMallocHost(&h_pairs,       pairsBytes);
        cudaMallocHost(&h_numPairs,    numPairBytes);
        cudaMallocHost(&h_pairOffsets, offsetBytes);
        cudaMallocHost(&h_decBits,     pairsBytes / 2);  // 1 decoded bit per pair
        cudaMallocHost(&h_decOffsets,  offsetBytes);

        // Per-packet staging: one slot per packet, each maxPairs*2 int8_t wide
        size_t stagingBytes = (size_t)batchSize * maxP * 2 * sizeof(int8_t);
        h_stagingBuf  = new int8_t[stagingBytes];
        h_stagingSlot = new int8_t*[batchSize];
        for (int i = 0; i < batchSize; i++)
            h_stagingSlot[i] = h_stagingBuf + (size_t)i * maxP * 2;

        // Persistent IO buffers
        sigInSize   = sigInSz;   cudaMallocHost(&sigInBuf,   sigInSz);
        dataInSize  = dataInSz;  cudaMallocHost(&dataInBuf,  dataInSz);
        sigOutSize  = sigOutSz;  cudaMallocHost(&sigOutBuf,  sigOutSz);
        dataOutSize = dataOutSz; cudaMallocHost(&dataOutBuf, dataOutSz);

        // Persistent per-batch scratch (plain heap — CPU-only, no need for pinned)
        lipArr    = new int[batchSize];
        encLipArr = new int[batchSize];
        errorArr  = new int[batchSize];

        cudaMalloc(&d_pairs,       pairsBytes);
        cudaMalloc(&d_numPairs,    numPairBytes);
        cudaMalloc(&d_pairOffsets, offsetBytes);
        cudaMalloc(&d_survivors,   survBytes);
        cudaMalloc(&d_decBits,     pairsBytes / 2);
        cudaMalloc(&d_decOffsets,  offsetBytes);

        cudaStreamCreate(&stream);

        // Create timing events once — reused every batch, never destroyed until free_all()
        cudaEventCreate(&evH2Dstart);
        cudaEventCreate(&evH2Ddone);
        cudaEventCreate(&evKdone);
        cudaEventCreate(&evD2Hdone);

        // Init timing accumulators
        timerBatch = 0;
        tH2D = tKernel = tD2H = 0.0f;
        QueryPerformanceFrequency(&tFreq);

        printf("[ChannelDecode] Survivors buffer (bit-packed): %.1f MB on GPU\n",
               survBytes / 1e6);
    }

    void free_all() {
        cudaEventDestroy(evH2Dstart);
        cudaEventDestroy(evH2Ddone);
        cudaEventDestroy(evKdone);
        cudaEventDestroy(evD2Hdone);
        cudaStreamDestroy(stream);
        cudaFreeHost(h_pairs);
        cudaFreeHost(h_numPairs);
        cudaFreeHost(h_pairOffsets);
        cudaFreeHost(h_decBits);
        cudaFreeHost(h_decOffsets);
        cudaFreeHost(sigInBuf);
        cudaFreeHost(dataInBuf);
        cudaFreeHost(sigOutBuf);
        cudaFreeHost(dataOutBuf);
        delete[] lipArr;
        delete[] encLipArr;
        delete[] errorArr;
        delete[] h_stagingBuf;
        delete[] h_stagingSlot;
        cudaFree(d_pairs);
        cudaFree(d_numPairs);
        cudaFree(d_pairOffsets);
        cudaFree(d_survivors);
        cudaFree(d_decBits);
        cudaFree(d_decOffsets);
    }
};

// -----------------------------------------------------------------------
// Block state
// -----------------------------------------------------------------------
struct ChannelDecodeData {
    ACSEntry  acsTable [NUM_STATES * 2];
    PredEntry predTable[NUM_STATES * 2];
    int frameCount  = 0;
    int errorFrames = 0;

    std::shared_ptr<GpuBufs> gpuBufs;
};

// -----------------------------------------------------------------------
// Init
// -----------------------------------------------------------------------
ChannelDecodeData init_channel_decode(const BlockConfig& config) {
    ChannelDecodeData data;
    build_trellis(data.acsTable, data.predTable);
    upload_trellis_to_gpu(data.predTable);

    int devCount = 0;
    cudaGetDeviceCount(&devCount);
    if (devCount == 0) {
        fprintf(stderr, "[ChannelDecode] ERROR: No CUDA GPU found\n");
        exit(1);
    }
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    printf("[ChannelDecode] GPU: %s  SM: %d.%d  Mem: %.0f MB\n",
           prop.name, prop.major, prop.minor,
           prop.totalGlobalMem / 1e6);

    // Detect OpenMP thread count
    int nThreads = omp_get_max_threads();
    printf("[ChannelDecode] OpenMP depuncture threads: %d\n", nThreads);

    int batchSize = config.inputBatchSizes[0];
    int sigInSz   = (config.inputPacketSizes[1]  * batchSize) + 4;
    int dataInSz  = (config.inputPacketSizes[0]  * batchSize) + 4;
    int sigOutSz  = (config.outputPacketSizes[1] * batchSize) + 4;
    int dataOutSz = (config.outputPacketSizes[0] * batchSize) + 4;

    data.gpuBufs  = std::shared_ptr<GpuBufs>(
        new GpuBufs(),
        [](GpuBufs* g) { g->free_all(); delete g; }
    );
    data.gpuBufs->alloc(batchSize, DEC_BITS_MAX, nThreads,
                        sigInSz, dataInSz, sigOutSz, dataOutSz);

    printf("[ChannelDecode] GPU Viterbi ready: %d packets/batch, %d max pairs/pkt\n",
           batchSize, DEC_BITS_MAX);
    return data;
}

// -----------------------------------------------------------------------
// Process
// -----------------------------------------------------------------------
void process_channel_decode(
    const char** pipeIn,
    const char** pipeOut,
    ChannelDecodeData& customData,
    const BlockConfig& config
) {
    const int inDataPkt  = config.inputPacketSizes[0];
    const int inSigPkt   = config.inputPacketSizes[1];
    const int outDataPkt = config.outputPacketSizes[0];
    const int outSigPkt  = config.outputPacketSizes[1];

    PipeIO inData   (pipeIn[0],  inDataPkt,  config.inputBatchSizes[0]);
    PipeIO inSignal (pipeIn[1],  inSigPkt,   config.inputBatchSizes[1]);
    PipeIO outData  (pipeOut[0], outDataPkt, config.outputBatchSizes[0]);
    PipeIO outSignal(pipeOut[1], outSigPkt,  config.outputBatchSizes[1]);

    // Use persistent pinned buffers — no heap alloc/free per batch
    GpuBufs& gpu      = *customData.gpuBufs;
    int8_t* sigInBuf  = gpu.sigInBuf;
    int8_t* dataInBuf = gpu.dataInBuf;
    int8_t* sigOutBuf = gpu.sigOutBuf;
    int8_t* dataOutBuf= gpu.dataOutBuf;

    // Use persistent per-batch scratch arrays — no new/delete per call
    int* lipArr    = gpu.lipArr;
    int* encLipArr = gpu.encLipArr;
    int* errorArr  = gpu.errorArr;

    memset(sigOutBuf,  0, gpu.sigOutSize);
    memset(dataOutBuf, 0, gpu.dataOutSize);

    // ===== STEP 1: Read SIGNAL_ENCODED =====
    int actualCount = inSignal.read(sigInBuf);

    // Reset per-batch scratch (replaces new int[...]() zero-init)
    for (int i = 0; i < actualCount; i++) {
        lipArr[i]    = 1515;
        encLipArr[i] = inDataPkt - 3;
        errorArr[i]  = 0;
    }

    // ===== STEP 2: Decode SIGNAL (CPU, serial — only 24 pairs/pkt) =====
    for (int i = 0; i < actualCount; i++) {
        SignalBuf tb;

        const int sigOff    = i * inSigPkt;
        const int sigOutOff = i * outSigPkt;

        uint8_t sigBytes[6];
        for (int j = 0; j < 6; j++)
            sigBytes[j] = (uint8_t)((int32_t)sigInBuf[sigOff + j] + 128);

        for (int j = 0; j < 48; j++)
            tb.sigPairs[j] = get_bit(sigBytes, j);

        memset(tb.sigDecBits, 0, 24 * sizeof(int));
        cpu_viterbi_decode(tb.sigPairs, 24, tb.sigDecBits,
                           customData.predTable, tb.survivors);

        uint8_t decSig[3] = {0, 0, 0};
        for (int b = 0; b < 24; b++)
            decSig[b >> 3] |= (uint8_t)(tb.sigDecBits[b] << (b & 7));

        uint8_t  rateVal = decSig[0] & 0x0F;
        uint16_t length  = (uint16_t)((decSig[0] >> 5) & 0x07)
                         | ((uint16_t)decSig[1] << 3)
                         | (((uint16_t)(decSig[2] & 0x01)) << 11);

        if (length == 0 || length > 4095) length = 1504;

        int ndbps = 48;
        switch (rateVal) {
            case 13: ndbps =  24; break; case 15: ndbps =  36; break;
            case  5: ndbps =  48; break; case  7: ndbps =  72; break;
            case  9: ndbps =  96; break; case 11: ndbps = 144; break;
            case  1: ndbps = 192; break; case  3: ndbps = 216; break;
        }

        int dbits   = 16 + (int)length * 8 + 6;
        int nsym    = (dbits + ndbps - 1) / ndbps;
        int padbits = nsym * ndbps - dbits;
        int tpbytes = (6 + padbits + 7) / 8;
        int lip     = 3 + 2 + (int)length + tpbytes;
        if (lip < 5)    lip = 5;
        if (lip > 1515) lip = 1515;
        lipArr[i] = lip;

        for (int j = 0; j < 3; j++)
            sigOutBuf[sigOutOff + j] = (int8_t)((int32_t)decSig[j] - 128);
    }

    // ===== STEP 3: Send SIGNAL =====
    outSignal.write(sigOutBuf, actualCount);

    // ===== STEP 4: Read rate+lip+DATA =====
    inData.read(dataInBuf);

    for (int i = 0; i < actualCount; i++) {
        const int off = i * inDataPkt;
        uint8_t lo = (uint8_t)((int32_t)dataInBuf[off + 1] + 128);
        uint8_t hi = (uint8_t)((int32_t)dataInBuf[off + 2] + 128);
        int encLip = (int)lo | ((int)hi << 8);
        if (encLip < 0)              encLip = 0;
        if (encLip > inDataPkt - 3) encLip = inDataPkt - 3;
        encLipArr[i] = encLip;
    }

    // ===== STEP 5: Single-pass depuncture — build compact pairs buffer =====
    //
    // Pass 1 (parallel): each packet i depunctures directly into its own
    //   staging slot h_stagingSlot[i]. No thread aliasing possible — each
    //   packet owns its slot exclusively. Records numPairs per packet.
    //
    // Serial prefix-sum: compute compacted byte offsets from numPairs.
    //
    // Pass 2 (parallel): memcpy staging slot[i] → h_pairs + pairOffsets[i].
    //   Fully parallel, no dependencies.

    // Pass 1: depuncture into per-packet staging slot
    #pragma omp parallel for schedule(dynamic, 4) num_threads(gpu.numOmpThreads)
    for (int i = 0; i < actualCount; i++) {
        int8_t* slot = gpu.h_stagingSlot[i];

        const int off          = i * inDataPkt;
        uint8_t   rateVal      = (uint8_t)((int32_t)dataInBuf[off] + 128);
        int       rateCode     = codingRateCode(rateVal);
        int       encDataBytes = encLipArr[i];
        int       numEncBits   = encDataBytes * 8;

        uint8_t encRaw[3024];
        for (int j = 0; j < encDataBytes; j++)
            encRaw[j] = (uint8_t)((int32_t)dataInBuf[off + 3 + j] + 128);

        int numPairs = depuncture_to_int8(encRaw, numEncBits, slot, rateCode);
        if (numPairs > gpu.maxPairs) { numPairs = gpu.maxPairs; errorArr[i] = 1; }
        gpu.h_numPairs[i] = numPairs;
    }

    // Serial prefix-sum to build compacted offsets (microseconds)
    gpu.h_pairOffsets[0] = 0;
    gpu.h_decOffsets[0]  = 0;
    for (int i = 0; i < actualCount; i++) {
        gpu.h_pairOffsets[i + 1] = gpu.h_pairOffsets[i] + gpu.h_numPairs[i] * 2;
        gpu.h_decOffsets [i + 1] = gpu.h_decOffsets [i] + gpu.h_numPairs[i];
    }

    // Pass 2 (parallel): memcpy staging → compacted h_pairs
    #pragma omp parallel for schedule(static) num_threads(gpu.numOmpThreads)
    for (int i = 0; i < actualCount; i++) {
        memcpy(gpu.h_pairs + gpu.h_pairOffsets[i],
               gpu.h_stagingSlot[i],
               (size_t)gpu.h_numPairs[i] * 2 * sizeof(int8_t));
    }

    // ===== STEP 6: GPU Viterbi — compact async H2D, kernel, compact async D2H =====
    size_t pairsBytes    = (size_t)gpu.h_pairOffsets[actualCount] * sizeof(int8_t);
    size_t numPairBytes  = (size_t)actualCount * sizeof(int);
    size_t offsetBytes   = (size_t)(actualCount + 1) * sizeof(int);
    size_t decBytesSize  = (size_t)gpu.h_decOffsets[actualCount] * sizeof(int8_t);

    // ---- Timing: only pay QPC cost on the batch before a print ----
    const bool doTiming = ((gpu.timerBatch % 200) == 199);
    if (doTiming) QueryPerformanceCounter(&gpu.tBatchStart);

    cudaEventRecord(gpu.evH2Dstart, gpu.stream);
    cudaMemcpyAsync(gpu.d_pairs,       gpu.h_pairs,       pairsBytes,   cudaMemcpyHostToDevice, gpu.stream);
    cudaMemcpyAsync(gpu.d_numPairs,    gpu.h_numPairs,    numPairBytes, cudaMemcpyHostToDevice, gpu.stream);
    cudaMemcpyAsync(gpu.d_pairOffsets, gpu.h_pairOffsets, offsetBytes,  cudaMemcpyHostToDevice, gpu.stream);
    cudaMemcpyAsync(gpu.d_decOffsets,  gpu.h_decOffsets,  offsetBytes,  cudaMemcpyHostToDevice, gpu.stream);
    cudaEventRecord(gpu.evH2Ddone, gpu.stream);

    dim3 grid(actualCount);
    dim3 block(NUM_STATES);
    viterbi_kernel<<<grid, block, 0, gpu.stream>>>(
        gpu.d_pairs, gpu.d_numPairs, gpu.d_pairOffsets,
        gpu.d_survivors,
        gpu.d_decBits, gpu.d_decOffsets,
        gpu.maxPairs, gpu.wordsPerPkt
    );
    cudaEventRecord(gpu.evKdone, gpu.stream);

    cudaMemcpyAsync(gpu.h_decBits, gpu.d_decBits, decBytesSize, cudaMemcpyDeviceToHost, gpu.stream);
    cudaEventRecord(gpu.evD2Hdone, gpu.stream);
    cudaStreamSynchronize(gpu.stream);

    float msH2D = 0, msKernel = 0, msD2H = 0;
    cudaEventElapsedTime(&msH2D,    gpu.evH2Dstart, gpu.evH2Ddone);
    cudaEventElapsedTime(&msKernel, gpu.evH2Ddone,  gpu.evKdone);
    cudaEventElapsedTime(&msD2H,    gpu.evKdone,    gpu.evD2Hdone);
    gpu.tH2D += msH2D; gpu.tKernel += msKernel; gpu.tD2H += msD2H;

    gpu.timerBatch++;
    if (gpu.timerBatch % 200 == 0) {
        float n = 200.0f;
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double msBatch = doTiming
            ? (now.QuadPart - gpu.tBatchStart.QuadPart) * 1000.0 / gpu.tFreq.QuadPart
            : 0.0;

        FILE* f = fopen("C:/timing_log.txt", "a");
        if (!f) f = fopen("timing_log.txt", "a");
        if (f) {
            fprintf(f, "[TIMING] batch=%d pkts=%d\n", gpu.timerBatch, actualCount);
            fprintf(f, "  H2D    : %.3f ms  (%.1f KB)\n", gpu.tH2D/n, pairsBytes/1024.0f);
            fprintf(f, "  Kernel : %.3f ms\n", gpu.tKernel/n);
            fprintf(f, "  D2H    : %.3f ms  (%.1f KB)\n", gpu.tD2H/n, decBytesSize/1024.0f);
            fprintf(f, "  GPU tot: %.3f ms\n", (gpu.tH2D+gpu.tKernel+gpu.tD2H)/n);
            fprintf(f, "  Batch  : %.3f ms  => %.1f Mbps\n",
                    msBatch,
                    msBatch > 0 ? (actualCount * 1500.0 * 8.0) / (msBatch / 1000.0) / 1e6 : 0.0);
            fprintf(f, "  pairs  : %zu bytes (%.0f%% of max)\n",
                    pairsBytes, 100.0*pairsBytes/((size_t)actualCount*gpu.maxPairs*2));
            fprintf(f, "\n");
            fclose(f);
        }
        gpu.tH2D = gpu.tKernel = gpu.tD2H = 0.0f;
    }

    // ===== STEP 7: Pack decoded bits -> output buffer (parallel) =====
    #pragma omp parallel for schedule(static) num_threads(gpu.numOmpThreads)
    for (int i = 0; i < actualCount; i++) {
        const int  dataOutOff = i * outDataPkt;
        const int  frameLip   = lipArr[i];
        const int8_t* decBits = gpu.h_decBits + gpu.h_decOffsets[i];  // compacted

        int numDecBytes = frameLip;
        if (numDecBytes > outDataPkt - 2) numDecBytes = outDataPkt - 2;

        uint8_t decBytes[1517] = {};

        const int sigOutOff = i * outSigPkt;
        for (int j = 0; j < 3 && j < numDecBytes; j++)
            decBytes[j] = (uint8_t)((int32_t)sigOutBuf[sigOutOff + j] + 128);

        int dfc = frameLip - 3;
        if (dfc < 0) dfc = 0;
        if (dfc > outDataPkt - 2 - 3) dfc = outDataPkt - 2 - 3;
        pack_decbits_to_bytes(decBits, dfc * 8, decBytes + 3, dfc);

        dataOutBuf[dataOutOff + 0] = (int8_t)((int32_t)(frameLip & 0xFF)        - 128);
        dataOutBuf[dataOutOff + 1] = (int8_t)((int32_t)((frameLip >> 8) & 0xFF) - 128);
        for (int j = 0; j < numDecBytes; j++)
            dataOutBuf[dataOutOff + 2 + j] = (int8_t)((int32_t)decBytes[j] - 128);
    }

    // ===== STEP 8: Send DATA =====
    outData.write(dataOutBuf, actualCount);

    int batchErrors = 0;
    for (int i = 0; i < actualCount; i++) batchErrors += errorArr[i];
    customData.errorFrames += batchErrors;
    customData.frameCount  += actualCount;

    // NOTE: No delete[] here — arrays are persistent in GpuBufs, freed in free_all()
}

// -----------------------------------------------------------------------
// main
// -----------------------------------------------------------------------
int main(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr,
            "Usage: channel_decode <pipeInData> <pipeInSignal>"
            " <pipeOutData> <pipeOutSignal>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1], argv[2]};
    const char* pipeOuts[] = {argv[3], argv[4]};


    BlockConfig config = {
        "ChannelDecode",
        2,               // inputs
        2,               // outputs
        {3027, 6},       // inputPacketSizes  [rate+lip+DATA_DEINT(3027), SIGNAL_ENC(6)]
        {64, 64},    // inputBatchSizes
        {1517, 3},       // outputPacketSizes [lip+DATA(1517), SIGNAL(3)]
        {64, 64},    // outputBatchSizes
        true,            // ltr
        true,            // startWithAll
        "GPU Viterbi decoder: SIGNAL on CPU, DATA on CUDA"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_channel_decode, init_channel_decode);
    return 0;
}

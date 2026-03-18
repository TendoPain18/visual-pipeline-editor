function channel_decode(pipeInSignal, pipeInData, pipeOutSignal, pipeOutData)
% CHANNEL_DECODE  IEEE 802.11a Viterbi decoder — batch processing
%
% Pipe sizes match channel_encode_middleman output / cpp descrambler input:
%   Input 1  (pipeInSignal):  6 bytes/pkt    × 44740 batch  — encoded SIGNAL (48 bits @ R=1/2)
%   Input 2  (pipeInData):    3025 bytes/pkt × 44740 batch  — rate(1) + encoded DATA
%   Output 1 (pipeOutSignal): 3 bytes/pkt    × 44740 batch  — decoded SIGNAL (24 bits = 3 bytes)
%   Output 2 (pipeOutData):   1515 bytes/pkt × 44740 batch  — decoded DATA (no rate byte)
%
% Per-frame deadlock-free read order (mirrors middleman write order):
%   1. Read SIGNAL batch  → decode → write SIGNAL batch  (unblocks ppdu_decapsulate)
%   2. Read DATA batch    → decode → write DATA batch

    block_config = struct( ...
        'name',              'ChannelDecode', ...
        'inputs',            2, ...
        'outputs',           2, ...
        'inputPacketSizes',  [6,     3025], ...
        'inputBatchSizes',   [44740, 44740], ...
        'outputPacketSizes', [3,     1515], ...
        'outputBatchSizes',  [44740, 44740], ...
        'LTR',               true, ...
        'startWithAll',      true, ...
        'socketHost',        'localhost', ...
        'socketPort',        9001 ...
    );

    run_generic_block( ...
        {pipeInSignal, pipeInData}, ...
        {pipeOutSignal, pipeOutData}, ...
        block_config, @process_fn, @init_fn);
end

%% ─── Init ────────────────────────────────────────────────────────────────────
function lut = init_fn(~)
    g0 = [1 0 1 1 0 1 1];
    g1 = [1 1 1 1 0 0 1];
    numStates = 64;

    nextState = zeros(numStates, 2);
    outBits   = zeros(numStates, 2, 2);
    for state = 0:numStates-1
        stateBits = de2bi(state, 6, 'right-msb');
        for inBit = 0:1
            newReg = [inBit, stateBits];
            outA   = mod(sum(newReg .* g0), 2);
            outB   = mod(sum(newReg .* g1), 2);
            nextSt = bi2de(newReg(1:6), 'right-msb');
            nextState(state+1, inBit+1)  = nextSt;
            outBits(state+1, inBit+1, :) = [outA, outB];
        end
    end

    rateMap = containers.Map( ...
        [13, 15, 5, 7, 9, 11, 1, 3], ...
        {'1/2','1/2','2/3','2/3','3/4','3/4','2/3','3/4'});

    depunctureMap = containers.Map( ...
        {'1/2','2/3','3/4'}, ...
        {[], [1 1; 1 0], [1 1 0; 1 0 1]});

    lut = struct( ...
        'numStates',     numStates, ...
        'nextState',     nextState, ...
        'outBits',       outBits, ...
        'rateMap',       rateMap, ...
        'depunctureMap', depunctureMap ...
    );
    fprintf('[ChannelDecode] Trellis built (%d states)\n', numStates);
end

%% ─── Process — called once per batch of 44740 frames ────────────────────────
function process_fn(pipeIn, pipeOut, lut, ~)
    IN_SIG_PKT   = pipeIn{1}.getPacketSize();   % 6
    IN_DATA_PKT  = pipeIn{2}.getPacketSize();   % 3025
    OUT_SIG_PKT  = pipeOut{1}.getPacketSize();  % 3
    OUT_DATA_PKT = pipeOut{2}.getPacketSize();  % 1515
    BATCH        = pipeIn{1}.getBatchSize();    % 44740

    % STEP 1: Read SIGNAL batch
    [sigRaw, actualCount] = pipeIn{1}.read();

    % Decode all SIGNAL frames and write immediately (unblocks ppdu_decapsulate)
    sigOut = zeros(BATCH * OUT_SIG_PKT, 1, 'int8');
    for i = 1:actualCount
        sOff = (i-1) * IN_SIG_PKT;
        soOff = (i-1) * OUT_SIG_PKT;

        sigBytes = uint8(int32(sigRaw(sOff+1 : sOff+6)) + 128);
        sigBits  = bytes2bits(sigBytes);                          % 48 bits
        decSig   = viterbi_decode(sigBits, lut);                  % 24 bits
        sigBytes3 = bits2bytes_n(decSig, 3);                      % 3 bytes
        sigOut(soOff+1 : soOff+3) = int8(int32(sigBytes3) - 128);
    end
    pipeOut{1}.write(sigOut, actualCount);

    % STEP 2: Read DATA batch
    [dataRaw, ~] = pipeIn{2}.read();

    % Decode all DATA frames
    dataOut = zeros(BATCH * OUT_DATA_PKT, 1, 'int8');
    for i = 1:actualCount
        dOff  = (i-1) * IN_DATA_PKT;
        doOff = (i-1) * OUT_DATA_PKT;

        rateValue       = uint8(int32(dataRaw(dOff+1)) + 128);
        dataBytes       = uint8(int32(dataRaw(dOff+2 : dOff+IN_DATA_PKT)) + 128);

        dataRateStr     = lut.rateMap(double(rateValue));
        depunct_pattern = lut.depunctureMap(dataRateStr);

        dataBits = bytes2bits(dataBytes);
        if ~isempty(depunct_pattern)
            dataBits = depuncture_bits(dataBits, depunct_pattern);
        end

        decData     = viterbi_decode(dataBits, lut);
        numOutBytes = floor(numel(decData) / 8);
        if numOutBytes > OUT_DATA_PKT; numOutBytes = OUT_DATA_PKT; end
        outBytes    = bits2bytes_n(decData, numOutBytes);

        dataOut(doOff+1 : doOff+numOutBytes) = int8(int32(outBytes) - 128);
    end
    pipeOut{2}.write(dataOut, actualCount);
end

%% ─── Helpers ─────────────────────────────────────────────────────────────────
function bits = bytes2bits(byteVec)
    n    = numel(byteVec);
    bits = zeros(1, n * 8);
    for i = 1:n
        bits((i-1)*8+1 : i*8) = de2bi(byteVec(i), 8, 'right-msb');
    end
end

function byteVec = bits2bytes_n(bits, n)
    byteVec = zeros(n, 1, 'uint8');
    for i = 1:n
        byteVec(i) = bi2de(bits((i-1)*8+1 : i*8), 'right-msb');
    end
end

function decodedBits = viterbi_decode(receivedBits, lut)
    numPairs   = floor(numel(receivedBits) / 2);
    pathMetric = inf(lut.numStates, 1);
    pathMetric(1) = 0;
    survivors  = zeros(lut.numStates, numPairs);

    for t = 1:numPairs
        recvA     = receivedBits((t-1)*2 + 1);
        recvB     = receivedBits((t-1)*2 + 2);
        newMetric = inf(lut.numStates, 1);

        for state = 0:lut.numStates-1
            for inBit = 0:1
                expA   = lut.outBits(state+1, inBit+1, 1);
                expB   = lut.outBits(state+1, inBit+1, 2);
                branch = 0;
                if recvA ~= -1; branch = branch + (recvA ~= expA); end
                if recvB ~= -1; branch = branch + (recvB ~= expB); end

                nextSt    = lut.nextState(state+1, inBit+1);
                candidate = pathMetric(state+1) + branch;
                if candidate < newMetric(nextSt+1)
                    newMetric(nextSt+1)    = candidate;
                    survivors(nextSt+1, t) = state * 2 + inBit;
                end
            end
        end
        pathMetric = newMetric;
    end

    [~, finalState] = min(pathMetric);
    finalState  = finalState - 1;
    decodedBits = zeros(1, numPairs);
    cur         = finalState;
    for t = numPairs:-1:1
        si             = survivors(cur+1, t);
        inBit          = mod(si, 2);
        cur            = floor(si / 2);
        decodedBits(t) = inBit;
    end
end

function out = depuncture_bits(bits, pattern)
    [~, pCols]  = size(pattern);
    numPairs    = floor(numel(bits) / sum(pattern(:)));
    out         = zeros(1, numPairs * 2 * pCols);
    outIdx = 1; inIdx = 1;
    for pair = 1:numPairs
        for col = 1:pCols
            if pattern(1,col); out(outIdx) = bits(inIdx); inIdx = inIdx+1;
            else;               out(outIdx) = -1; end
            outIdx = outIdx + 1;
            if pattern(2,col); out(outIdx) = bits(inIdx); inIdx = inIdx+1;
            else;               out(outIdx) = -1; end
            outIdx = outIdx + 1;
        end
    end
    out = out(1:outIdx-1);
end

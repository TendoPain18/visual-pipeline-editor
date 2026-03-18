function channel_encode(pipeInRate, pipeInData, pipeOutRate, pipeOutData)
% CHANNEL_ENCODE  IEEE 802.11a Convolutional Encoder — batch processing
%
% Sits after cpp scrambler, before channel_encode_middleman.
%
% Pipe sizes match C++ scrambler output / middleman input:
%   Input 1  (pipeInRate):  3 bytes/pkt  × 44740 batch  (rate + length uint16 LE)
%   Input 2  (pipeInData):  1515 bytes/pkt × 44740 batch (SIGNAL[3] + DATA[variable])
%   Output 1 (pipeOutRate): 3 bytes/pkt  × 44740 batch  (rate + encoded_length uint16)
%   Output 2 (pipeOutData): 3030 bytes/pkt × 44740 batch (encoded SIGNAL + encoded DATA)
%
% Per frame:
%   SIGNAL (bytes 1-3, 24 bits) always encoded @ R=1/2 → 6 bytes (48 bits)
%   DATA   (bytes 4..3+dataLength) encoded at rate from RATE byte

    block_config = struct( ...
        'name',              'ChannelEncode', ...
        'inputs',            2, ...
        'outputs',           2, ...
        'inputPacketSizes',  [3,    1515], ...
        'inputBatchSizes',   [44740, 44740], ...
        'outputPacketSizes', [3,    3030], ...
        'outputBatchSizes',  [44740, 44740], ...
        'LTR',               true, ...
        'startWithAll',      true, ...
        'socketHost',        'localhost', ...
        'socketPort',        9001 ...
    );

    run_generic_block( ...
        {pipeInRate, pipeInData}, ...
        {pipeOutRate, pipeOutData}, ...
        block_config, @process_fn, @init_fn);
end

%% ─── Init ────────────────────────────────────────────────────────────────────
function lut = init_fn(~)
    g0 = [1 0 1 1 0 1 1];
    g1 = [1 1 1 1 0 0 1];

    rateMap = containers.Map( ...
        [13, 15, 5, 7, 9, 11, 1, 3], ...
        {'1/2','1/2','2/3','2/3','3/4','3/4','2/3','3/4'});

    punctureMap = containers.Map( ...
        {'1/2','2/3','3/4'}, ...
        {[], [1 1; 1 0], [1 1 0; 1 0 1]});

    lut = struct('g0', g0, 'g1', g1, 'rateMap', rateMap, 'punctureMap', punctureMap);
    fprintf('[ChannelEncode] Init done\n');
end

%% ─── Process — called once per batch of 44740 frames ────────────────────────
function process_fn(pipeIn, pipeOut, lut, ~)
    IN_RATE_PKT  = pipeIn{1}.getPacketSize();   % 3
    IN_DATA_PKT  = pipeIn{2}.getPacketSize();   % 1515
    OUT_RATE_PKT = pipeOut{1}.getPacketSize();  % 3
    OUT_DATA_PKT = pipeOut{2}.getPacketSize();  % 3030
    BATCH        = pipeIn{1}.getBatchSize();    % 44740

    % ── Read full batches ─────────────────────────────────────────────────────
    [rateRaw, actualCount] = pipeIn{1}.read();
    [dataRaw, ~]           = pipeIn{2}.read();

    % ── Allocate output batches ───────────────────────────────────────────────
    rateOut = zeros(BATCH * OUT_RATE_PKT, 1, 'int8');
    dataOut = zeros(BATCH * OUT_DATA_PKT, 1, 'int8');

    % ── Process each frame ────────────────────────────────────────────────────
    for i = 1:actualCount
        rOff = (i-1) * IN_RATE_PKT;
        dOff = (i-1) * IN_DATA_PKT;
        roOff = (i-1) * OUT_RATE_PKT;
        doOff = (i-1) * OUT_DATA_PKT;

        % Unpack rate_length (3 bytes)
        rBytes     = uint8(int32(rateRaw(rOff+1 : rOff+3)) + 128);
        rateValue  = rBytes(1);
        dataLength = uint16(rBytes(2)) + uint16(bitshift(uint16(rBytes(3)), 8));

        % Unpack SIGNAL + DATA
        dBytes = uint8(int32(dataRaw(dOff+1 : dOff+IN_DATA_PKT)) + 128);

        if ~isKey(lut.rateMap, double(rateValue))
            error('rateValue %d not found in rateMap. Available keys: %s', ...
                    double(rateValue), mat2str(cell2mat(keys(lut.rateMap))));
        end

        % Coding rate lookup
        dataRateStr      = lut.rateMap(double(rateValue));
        puncture_pattern = lut.punctureMap(dataRateStr);

        % Encode SIGNAL (bytes 1-3 = 24 bits → 48 bits @ R=1/2)
        signalBits    = bytes2bits(dBytes(1:3));
        signalEncoded = conv_encode_half(signalBits, lut.g0, lut.g1);  % 48 bits

        % Encode DATA (bytes 4..3+dataLength)
        dLen = double(dataLength);
        if dLen < 1; dLen = 1; end
        if 3+dLen > IN_DATA_PKT; dLen = IN_DATA_PKT - 3; end
        dataBits    = bytes2bits(dBytes(4 : 3+dLen));
        dataEncoded = conv_encode_puncture(dataBits, lut.g0, lut.g1, puncture_pattern);

        % Combine and pack to bytes
        allBits = [signalEncoded, dataEncoded];
        if mod(numel(allBits), 8) ~= 0
            allBits = [allBits, zeros(1, 8 - mod(numel(allBits), 8))];
        end
        numOutBytes = numel(allBits) / 8;
        outBytes    = bits2bytes(allBits);   % uint8 column

        % Pack rate output (3 bytes)
        rateOut(roOff+1) = int8(int32(rateValue) - 128);
        rateOut(roOff+2) = int8(int32(bitand(numOutBytes, 255)) - 128);
        rateOut(roOff+3) = int8(int32(bitshift(numOutBytes, -8)) - 128);

        % Pack data output (up to 3030 bytes per frame)
        copyLen = min(numOutBytes, OUT_DATA_PKT);
        dataOut(doOff+1 : doOff+copyLen) = int8(int32(outBytes(1:copyLen)) - 128);
    end

    % ── Write ─────────────────────────────────────────────────────────────────
    pipeOut{1}.write(rateOut, actualCount);
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

function byteVec = bits2bytes(bits)
    n       = numel(bits) / 8;
    byteVec = zeros(n, 1, 'uint8');
    for i = 1:n
        byteVec(i) = bi2de(bits((i-1)*8+1 : i*8), 'right-msb');
    end
end

function enc = conv_encode_half(bits, g0, g1)
    n   = numel(bits);
    enc = zeros(1, n * 2);
    sr  = zeros(1, 7);
    for i = 1:n
        sr             = [bits(i), sr(1:6)];
        enc((i-1)*2+1) = mod(sum(sr .* g0), 2);
        enc((i-1)*2+2) = mod(sum(sr .* g1), 2);
    end
end

function enc = conv_encode_puncture(bits, g0, g1, puncture_pattern)
    n  = numel(bits);
    sr = zeros(1, 7);
    if isempty(puncture_pattern)
        enc    = zeros(1, n * 2);
        outIdx = 1;
        for i = 1:n
            sr             = [bits(i), sr(1:6)];
            enc(outIdx)    = mod(sum(sr .* g0), 2);
            enc(outIdx+1)  = mod(sum(sr .* g1), 2);
            outIdx         = outIdx + 2;
        end
    else
        [~, pCols] = size(puncture_pattern);
        enc        = zeros(1, n * 2);
        outIdx     = 1;
        pIdx       = 0;
        for i = 1:n
            sr   = [bits(i), sr(1:6)];
            outA = mod(sum(sr .* g0), 2);
            outB = mod(sum(sr .* g1), 2);
            col  = mod(pIdx, pCols) + 1;
            if puncture_pattern(1, col); enc(outIdx) = outA; outIdx = outIdx+1; end
            if puncture_pattern(2, col); enc(outIdx) = outB; outIdx = outIdx+1; end
            pIdx = pIdx + 1;
        end
        enc = enc(1:outIdx-1);
    end
end

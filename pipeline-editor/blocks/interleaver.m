function interleaver(pipeInRate, pipeInData, pipeOutRate, pipeOutData)
% INTERLEAVER_FIXED - IEEE 802.11a Data Interleaver
%
% Input 1: rate_encoded_length (3 bytes: rate + encoded length uint16)
% Input 2: encoded data (SIGNAL + DATA, max 3030 bytes)
% Output 1: rate_encoded_length (3 bytes, passthrough)
% Output 2: interleaved data
%
% @BlockConfig
% name: Interleaver
% inputs: 2
% outputs: 2
% inputSize: [3, 3030]
% outputSize: [3, 3030]
% LTR: true
% startWithAll: true
% description: IEEE 802.11a interleaver with length extraction
% @EndBlockConfig

    config = parse_block_config();
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');

    try
        % RATE to modulation parameters
        rateParams = containers.Map(...
            [13, 15, 5, 7, 9, 11, 1, 3], ...
            {struct('NBPSC', 1, 'NCBPS', 48), ...   % 6 Mbps
             struct('NBPSC', 1, 'NCBPS', 48), ...   % 9 Mbps
             struct('NBPSC', 2, 'NCBPS', 96), ...   % 12 Mbps
             struct('NBPSC', 2, 'NCBPS', 96), ...   % 18 Mbps
             struct('NBPSC', 4, 'NCBPS', 192), ...  % 24 Mbps
             struct('NBPSC', 4, 'NCBPS', 192), ...  % 36 Mbps
             struct('NBPSC', 6, 'NCBPS', 288), ...  % 48 Mbps
             struct('NBPSC', 6, 'NCBPS', 288)});    % 54 Mbps
        
        send_protocol_message('BLOCK_READY', config.blockId, config.name, '');
        
        frameCount = 0;
        totalBytes = 0;
        startTime = tic;
        lastTime = 0;
        lastBytes = 0;
        
        while true
            frameCount = frameCount + 1;
            
            % Read rate_encoded_length
            rateEncodedLengthInput = pipeline_mex('read', pipeInRate, config.inputSize(1));
            rateValue = uint8(int32(rateEncodedLengthInput(1)) + 128);
            encodedLengthLow = uint8(int32(rateEncodedLengthInput(2)) + 128);
            encodedLengthHigh = uint8(int32(rateEncodedLengthInput(3)) + 128);
            encodedLength = uint16(encodedLengthLow) + bitshift(uint16(encodedLengthHigh), 8);
            
            % Read encoded data
            inputData = pipeline_mex('read', pipeInData, config.inputSize(2));
            dataBytes = uint8(int32(inputData(1:encodedLength)) + 128);
            
            % Convert to bits
            numBits = encodedLength * 8;
            inputBits = zeros(1, numBits);
            for i = 1:encodedLength
                bits = de2bi(dataBytes(i), 8, 'right-msb');
                inputBits((i-1)*8 + 1 : i*8) = bits;
            end
            
            % SIGNAL field - always BPSK (first 48 bits)
            signalBits = inputBits(1:48);
            NCBPS_SIGNAL = 48;
            NBPSC_SIGNAL = 1;
            s_SIGNAL = max(floor(NBPSC_SIGNAL / 2), 1);
            
            interleavedSignal = zeros(1, NCBPS_SIGNAL);
            for k = 0:(NCBPS_SIGNAL - 1)
                i = (NCBPS_SIGNAL / 16) * mod(k, 16) + floor(k / 16);
                j = s_SIGNAL * floor(i / s_SIGNAL) + mod(i + NCBPS_SIGNAL - floor(16 * i / NCBPS_SIGNAL), s_SIGNAL);
                interleavedSignal(j + 1) = signalBits(k + 1);
            end
            
            % DATA field - RATE-driven parameters
            dataBits = inputBits(49:end);
            params = rateParams(rateValue);
            NBPSC = params.NBPSC;
            NCBPS = params.NCBPS;
            s = max(floor(NBPSC / 2), 1);
            
            numDataBits = length(dataBits);
            numSymbols = floor(numDataBits / NCBPS);
            interleavedData = zeros(1, numDataBits);
            
            % Interleave symbol by symbol
            for sym = 1:numSymbols
                startIdx = (sym - 1) * NCBPS + 1;
                endIdx = sym * NCBPS;
                symbolBits = dataBits(startIdx:endIdx);
                interleavedSymbol = zeros(1, NCBPS);
                
                % Two-step interleaving per IEEE 802.11a
                for k = 0:(NCBPS - 1)
                    i = (NCBPS / 16) * mod(k, 16) + floor(k / 16);
                    j = s * floor(i / s) + mod(i + NCBPS - floor(16 * i / NCBPS), s);
                    interleavedSymbol(j + 1) = symbolBits(k + 1);
                end
                
                interleavedData(startIdx:endIdx) = interleavedSymbol;
            end
            
            % Handle remaining bits
            if numSymbols * NCBPS < numDataBits
                remainStart = numSymbols * NCBPS + 1;
                interleavedData(remainStart:end) = dataBits(remainStart:end);
            end
            
            % Combine SIGNAL and DATA
            outputBits = [interleavedSignal, interleavedData];
            
            % Convert to bytes
            numOutBytes = ceil(length(outputBits) / 8);
            if mod(length(outputBits), 8) ~= 0
                padBits = 8 - mod(length(outputBits), 8);
                outputBits = [outputBits, zeros(1, padBits)];
            end
            
            outputBytes = zeros(numOutBytes, 1, 'uint8');
            for i = 1:numOutBytes
                bitStart = (i-1) * 8 + 1;
                bitEnd = i * 8;
                outputBytes(i) = bi2de(outputBits(bitStart:bitEnd), 'right-msb');
            end
            
            % Prepare outputs
            dataOutput = zeros(config.outputSize(2), 1, 'int8');
            copyLen = min(numOutBytes, config.outputSize(2));
            dataOutput(1:copyLen) = int8(int32(outputBytes(1:copyLen)) - 128);
            
            % Write outputs
            pipeline_mex('write', pipeOutRate, rateEncodedLengthInput);
            pipeline_mex('write', pipeOutData, dataOutput);
            
            totalBytes = totalBytes + numOutBytes;
            
            currentTime = toc(startTime);
            elapsed = currentTime - lastTime;
            if elapsed > 0
                instantGbps = ((totalBytes - lastBytes) * 8 / 1e9) / elapsed;
            else
                instantGbps = 0;
            end
            lastTime = currentTime;
            lastBytes = totalBytes;
            
            metrics = struct();
            metrics.frames = frameCount;
            metrics.gbps = instantGbps;
            send_protocol_message('BLOCK_METRICS', config.blockId, config.name, metrics);
        end
        
    catch ME
        send_protocol_message('BLOCK_ERROR', config.blockId, config.name, ME.message);
        rethrow(ME);
    end
end

function config = parse_block_config()
    filePath = mfilename('fullpath');
    fid = fopen([filePath '.m'], 'r');
    if fid == -1, error('Cannot open configuration file'); end
    content = fread(fid, '*char')';
    fclose(fid);
    startMarker = '@BlockConfig';
    endMarker = '@EndBlockConfig';
    startIdx = strfind(content, startMarker);
    endIdx = strfind(content, endMarker);
    if isempty(startIdx) || isempty(endIdx), error('No @BlockConfig section found'); end
    configStart = startIdx(1) + length(startMarker);
    configEnd = endIdx(1) - 1;
    configText = content(configStart:configEnd);
    config = struct();
    lines = strsplit(configText, newline);
    for i = 1:length(lines)
        line = strtrim(lines{i});
        if isempty(line), continue; end
        if line(1) == '%', line = strtrim(line(2:end)); end
        if isempty(line), continue; end
        colonIdx = strfind(line, ':');
        if isempty(colonIdx), continue; end
        key = strtrim(line(1:colonIdx(1)-1));
        value = strtrim(line(colonIdx(1)+1:end));
        commentIdx = strfind(value, '%');
        if ~isempty(commentIdx), value = strtrim(value(1:commentIdx(1)-1)); end
        if isempty(key), continue; end
        try
            numValue = eval(value);
            config.(key) = numValue;
        catch
            config.(key) = value;
        end
    end
    blockIdStr = getenv('BLOCK_ID');
    if isempty(blockIdStr)
        config.blockId = 0;
    else
        config.blockId = str2double(blockIdStr);
    end
    requiredFields = {'name', 'inputs', 'outputs', 'inputSize', 'outputSize'};
    for i = 1:length(requiredFields)
        if ~isfield(config, requiredFields{i}), error('Missing required field: %s', requiredFields{i}); end
    end
end
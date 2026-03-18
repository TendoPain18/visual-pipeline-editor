function deinterleaver(pipeInSignal, pipeInData, pipeOutSignal, pipeOutData)
% DEINTERLEAVER_SIGNAL_DATA - DEBUG VERSION
%
% Input 1: signal_bits (6 bytes, 48 bits interleaved+encoded)
% Input 2: data_bits + rate (rate in first byte)
% Output 1: deinterleaved signal_bits (6 bytes)
% Output 2: deinterleaved data_bits + rate
%
% @BlockConfig
% name: Deinterleaver
% inputs: 2
% outputs: 2
% inputSize: [6, 3025]
% outputSize: [6, 3025]
% LTR: false
% startWithAll: true
% description: DEBUG - Deinterleaver with extensive logging
% @EndBlockConfig

    config = parse_block_config();
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');

    try
        % RATE to parameters
        rateParams = containers.Map(...
            [13, 15, 5, 7, 9, 11, 1, 3], ...
            {struct('NBPSC', 1, 'NCBPS', 48), ...
             struct('NBPSC', 1, 'NCBPS', 48), ...
             struct('NBPSC', 2, 'NCBPS', 96), ...
             struct('NBPSC', 2, 'NCBPS', 96), ...
             struct('NBPSC', 4, 'NCBPS', 192), ...
             struct('NBPSC', 4, 'NCBPS', 192), ...
             struct('NBPSC', 6, 'NCBPS', 288), ...
             struct('NBPSC', 6, 'NCBPS', 288)});
        
        send_protocol_message('BLOCK_READY', config.blockId, config.name, '');
        
        frameCount = 0;
        totalBytes = 0;
        startTime = tic;
        lastTime = 0;
        lastBytes = 0;
        
        while true
            frameCount = frameCount + 1;
            
            % ===== STEP 1: Wait for SIGNAL =====
            try
                signalInput = pipeline_mex('read', pipeInSignal, config.inputSize(1));
            catch ME
                fprintf('ERROR reading SIGNAL: %s\n', ME.message);
                rethrow(ME);
            end
            
            signalBytes = uint8(int32(signalInput) + 128);
            
            % Convert to bits
            signalBits = zeros(1, 48);
            for i = 1:6
                bits = de2bi(signalBytes(i), 8, 'right-msb');
                signalBits((i-1)*8 + 1 : i*8) = bits;
            end
            
            % Deinterleave SIGNAL (BPSK, NCBPS=48, NBPSC=1)
            NCBPS_SIGNAL = 48;
            NBPSC_SIGNAL = 1;
            s_SIGNAL = max(floor(NBPSC_SIGNAL / 2), 1);
            
            deinterleavedSignal = zeros(1, NCBPS_SIGNAL);
            for j = 0:(NCBPS_SIGNAL - 1)
                i = s_SIGNAL * floor(j / s_SIGNAL) + mod(j + floor(16 * j / NCBPS_SIGNAL), s_SIGNAL);
                k = 16 * i - (NCBPS_SIGNAL - 1) * floor(16 * i / NCBPS_SIGNAL);
                deinterleavedSignal(k + 1) = signalBits(j + 1);
            end
            
            % Convert back to bytes
            signalOutBytes = zeros(6, 1, 'uint8');
            for i = 1:6
                bitStart = (i-1) * 8 + 1;
                signalOutBytes(i) = bi2de(deinterleavedSignal(bitStart:bitStart+7), 'right-msb');
            end
            
            % Send SIGNAL output
            signalOutput = int8(int32(signalOutBytes) - 128);
            try
                pipeline_mex('write', pipeOutSignal, signalOutput);
            catch ME
                fprintf('ERROR writing SIGNAL: %s\n', ME.message);
                rethrow(ME);
            end
            
            % ===== STEP 2: Wait for DATA =====
            try
                dataInput = pipeline_mex('read', pipeInData, config.inputSize(2));
            catch ME
                fprintf('ERROR reading DATA: %s\n', ME.message);
                rethrow(ME);
            end
            
            % Extract rate
            rateValue = uint8(int32(dataInput(1)) + 128);
            dataBytes = uint8(int32(dataInput(2:end)) + 128);
            
            % Convert to bits
            numBytes = length(dataBytes);
            numBits = numBytes * 8;
            dataBits = zeros(1, numBits);
            for i = 1:numBytes
                bits = de2bi(dataBytes(i), 8, 'right-msb');
                dataBits((i-1)*8 + 1 : i*8) = bits;
            end
            
            % Deinterleave DATA based on RATE
            params = rateParams(rateValue);
            NBPSC = params.NBPSC;
            NCBPS = params.NCBPS;
            s = max(floor(NBPSC / 2), 1);
                        
            numSymbols = floor(numBits / NCBPS);
            
            deinterleavedData = zeros(1, numBits);
            
            for sym = 1:numSymbols
                startIdx = (sym - 1) * NCBPS + 1;
                endIdx = sym * NCBPS;
                symbolBits = dataBits(startIdx:endIdx);
                deinterleavedSymbol = zeros(1, NCBPS);
                
                for j = 0:(NCBPS - 1)
                    i = s * floor(j / s) + mod(j + floor(16 * j / NCBPS), s);
                    k = 16 * i - (NCBPS - 1) * floor(16 * i / NCBPS);
                    deinterleavedSymbol(k + 1) = symbolBits(j + 1);
                end
                
                deinterleavedData(startIdx:endIdx) = deinterleavedSymbol;
            end
            
            % Handle remaining bits
            if numSymbols * NCBPS < numBits
                remainStart = numSymbols * NCBPS + 1;
                deinterleavedData(remainStart:end) = dataBits(remainStart:end);
            end
            
            % Convert back to bytes
            dataOutBytes = zeros(numBytes, 1, 'uint8');
            for i = 1:numBytes
                bitStart = (i-1) * 8 + 1;
                dataOutBytes(i) = bi2de(deinterleavedData(bitStart:bitStart+7), 'right-msb');
            end
            
            % Send DATA output with rate prepended
            dataOutput = zeros(config.outputSize(2), 1, 'int8');
            dataOutput(1) = int8(int32(rateValue) - 128);
            copyLen = min(numBytes, config.outputSize(2) - 1);
            dataOutput(2:copyLen+1) = int8(int32(dataOutBytes(1:copyLen)) - 128);
            
            try
                pipeline_mex('write', pipeOutData, dataOutput);
            catch ME
                fprintf('ERROR writing DATA: %s\n', ME.message);
                rethrow(ME);
            end
                        
            totalBytes = totalBytes + numBytes;
            
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
        fprintf('\nDEINTERLEAVER FATAL ERROR in frame %d\n', frameCount);
        fprintf('Message: %s\n', ME.message);
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
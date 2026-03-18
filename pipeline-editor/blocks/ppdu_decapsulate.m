function ppdu_decapsulate(pipeInSignal, pipeInData, pipeOutData, pipeOutRateLength)
% PPDU_DECAPSULATE_FEEDBACK - DEBUG VERSION
%
% Input 1: signal (3 bytes, 24 bits decoded)
% Input 2: data (SERVICE + PSDU + TAIL+PAD, descrambled)
% Output 1: PSDU only (1504 bytes for CRC)
% Output 2: rate_length feedback (3 bytes: rate + length as uint16 LE)
%
% @BlockConfig
% name: PpduDecapsulate
% inputs: 2
% outputs: 2
% inputSize: [3, 1512]
% outputSize: [1504, 3]
% LTR: false
% startWithAll: true
% description: DEBUG - PPDU decapsulation with feedback logging
% @EndBlockConfig

    config = parse_block_config();
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');
    
    try
        % RATE value to Mbps (for logging)
        validRates = [13, 15, 5, 7, 9, 11, 1, 3];
        
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
            
            % Parse SIGNAL field (24 bits decoded)
            signal_byte1 = signalBytes(1);
            signal_byte2 = signalBytes(2);
            signal_byte3 = signalBytes(3);
                        
            % Extract RATE (bits 0-3)
            rate_value = bitand(signal_byte1, 15);
            
            % Validate RATE
            if ~ismember(rate_value, validRates)
                fprintf('ERROR: Invalid RATE value %d\n', rate_value);
                fprintf('Skipping corrupted frame\n');
                continue;
            end
            
            % Extract LENGTH (bits 5-16)
            length_bits_0_2 = uint16(bitshift(signal_byte1, -5));
            length_bits_3_10 = uint16(signal_byte2);
            length_bits_11 = uint16(bitand(signal_byte3, 1));
            psdu_length = length_bits_0_2 + bitshift(length_bits_3_10, 3) + bitshift(length_bits_11, 11);
            
            % Validate LENGTH
            if psdu_length > 4095
                fprintf('ERROR: Invalid LENGTH %d (exceeds 4095)\n', psdu_length);
                fprintf('Skipping corrupted frame\n');
                continue;
            end

            % Send rate_length feedback to adapter
            rateLengthOutput = zeros(3, 1, 'int8');
            rateLengthOutput(1) = int8(int32(rate_value) - 128);
            rateLengthOutput(2) = int8(int32(bitand(psdu_length, 255)) - 128);
            rateLengthOutput(3) = int8(int32(bitshift(psdu_length, -8)) - 128);
            
            try
                pipeline_mex('write', pipeOutRateLength, rateLengthOutput);
            catch ME
                fprintf('ERROR writing feedback: %s\n', ME.message);
                fprintf('This might mean InterleaverAdapter is not connected!\n');
                rethrow(ME);
            end
            
            % ===== STEP 2: Wait for DATA =====
            try
                dataInput = pipeline_mex('read', pipeInData, config.inputSize(2));
            catch ME
                fprintf('ERROR reading DATA: %s\n', ME.message);
                rethrow(ME);
            end
            
            dataBytes = uint8(int32(dataInput) + 128);
            
            % Extract PSDU (skip 2-byte SERVICE field)
            % Format: SERVICE(2 bytes) + PSDU(psdu_length bytes) + TAIL+PAD
            psdu_start = 3;
            psdu_end = psdu_start + psdu_length - 1;
            
            if psdu_end > length(dataBytes)
                fprintf('WARNING: PSDU length exceeds data! Expected %d, got %d\n', psdu_end, length(dataBytes));
                psdu_end = length(dataBytes);
            end
            
            psdu_bytes = dataBytes(psdu_start:psdu_end);
            
            % Send PSDU output (for CRC check)
            dataOutput = zeros(config.outputSize(1), 1, 'int8');
            copyLen = min(length(psdu_bytes), config.outputSize(1));
            dataOutput(1:copyLen) = int8(int32(psdu_bytes(1:copyLen)) - 128);
            
            try
                pipeline_mex('write', pipeOutData, dataOutput);
            catch ME
                fprintf('ERROR writing PSDU: %s\n', ME.message);
                rethrow(ME);
            end
                        
            totalBytes = totalBytes + length(psdu_bytes);
            
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
        fprintf('\nPPDU DECAPSULATE FATAL ERROR in frame %d\n', frameCount);
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
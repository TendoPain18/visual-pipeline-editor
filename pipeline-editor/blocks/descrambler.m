function descrambler(pipeInSignal, pipeInData, pipeOutSignal, pipeOutData)
% DESCRAMBLER_SIGNAL_DATA - DEBUG VERSION
%
% Input 1: signal (3 bytes, NOT scrambled)
% Input 2: data (SERVICE + PSDU + TAIL+PAD, scrambled)
% Output 1: signal (pass through)
% Output 2: descrambled data
%
% @BlockConfig
% name: Descrambler
% inputs: 2
% outputs: 2
% inputSize: [3, 1512]
% outputSize: [3, 1512]
% LTR: false
% startWithAll: true
% polynomial: S(x) = x^7 + x^4 + 1
% description: DEBUG - Descrambler with extensive logging
% @EndBlockConfig

    config = parse_block_config();
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');
    
    try
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
                        
            % Pass SIGNAL through unchanged
            try
                pipeline_mex('write', pipeOutSignal, signalInput);
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
            
            dataBytes = uint8(int32(dataInput) + 128);
            
            % Extract scrambler seed from SERVICE byte 1 (bits 0-6)
            service_byte1_scrambled = dataBytes(1);
            state_value = bitand(service_byte1_scrambled, 127);
            initial_state = de2bi(state_value, 7, 'left-msb');
                        
            % Descramble all bytes (SERVICE + PSDU + TAIL+PAD)
            scrambler_state = initial_state;
            descrambled_data = zeros(size(dataBytes), 'uint8');
            
            for byte_idx = 1:length(dataBytes)
                byte_val = dataBytes(byte_idx);
                byte_bits = de2bi(byte_val, 8, 'right-msb');
                descrambled_bits = zeros(1, 8);
                
                for bit_idx = 1:8
                    scrambler_output = scrambler_state(7);
                    descrambled_bits(bit_idx) = bitxor(byte_bits(bit_idx), scrambler_output);
                    
                    % Update: S(x) = x^7 + x^4 + 1
                    new_bit = bitxor(scrambler_state(7), scrambler_state(4));
                    scrambler_state = [new_bit, scrambler_state(1:6)];
                end
                
                descrambled_data(byte_idx) = bi2de(descrambled_bits, 'right-msb');
            end
            
            % Send DATA output
            dataOutput = zeros(config.outputSize(2), 1, 'int8');
            copyLen = min(length(descrambled_data), config.outputSize(2));
            dataOutput(1:copyLen) = int8(int32(descrambled_data(1:copyLen)) - 128);
            
            try
                pipeline_mex('write', pipeOutData, dataOutput);
            catch ME
                fprintf('ERROR writing DATA: %s\n', ME.message);
                rethrow(ME);
            end
                        
            totalBytes = totalBytes + length(descrambled_data);
            
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
        fprintf('\nDESCRAMBLER FATAL ERROR in frame %d\n', frameCount);
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
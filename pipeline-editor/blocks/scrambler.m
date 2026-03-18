function scrambler(pipeInRate, pipeInData, pipeOutRate, pipeOutData)
% SCRAMBLER_FIXED - Scrambles DATA only, passes SIGNAL through
%
% Input 1: rate_length (3 bytes: rate + data length as uint16 LE)
% Input 2: SIGNAL + DATA (max 1515 bytes)
% Output 1: rate_length (3 bytes, passthrough)
% Output 2: SIGNAL (unchanged, 3 bytes) + scrambled DATA
%
% @BlockConfig
% name: Scrambler
% inputs: 2
% outputs: 2
% inputSize: [3, 1515]
% outputSize: [3, 1515]
% LTR: true
% startWithAll: true
% polynomial: S(x) = x^7 + x^4 + 1
% description: Scrambler - SIGNAL passthrough, DATA scrambled with seed in SERVICE
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
            
            % Read rate_length (3 bytes)
            rateLengthInput = pipeline_mex('read', pipeInRate, config.inputSize(1));
            rateValue = uint8(int32(rateLengthInput(1)) + 128);
            lengthLowByte = uint8(int32(rateLengthInput(2)) + 128);
            lengthHighByte = uint8(int32(rateLengthInput(3)) + 128);
            dataLength = uint16(lengthLowByte) + uint16(bitshift(uint16(lengthHighByte), 8));
            
            % Read data
            inputData = pipeline_mex('read', pipeInData, config.inputSize(2));
            dataBytes = uint8(int32(inputData) + 128);
            
            % Extract SIGNAL (first 3 bytes, NOT scrambled)
            signal_field = dataBytes(1:3);
            
            % Extract DATA (SERVICE + PSDU + TAIL+PAD)
            % Total data length is in dataLength parameter
            data_field = dataBytes(4:3+dataLength);
            
            % Generate pseudo-random scrambler seed (1-127)
            seed = mod(frameCount * 37 + 127, 127) + 1;
            scrambler_state = de2bi(seed, 7, 'left-msb');
            
            % Scramble DATA
            scrambled_data = zeros(size(data_field), 'uint8');
            
            for byte_idx = 1:length(data_field)
                byte_val = data_field(byte_idx);
                byte_bits = de2bi(byte_val, 8, 'right-msb');
                scrambled_bits = zeros(1, 8);
                
                for bit_idx = 1:8
                    scrambler_output = scrambler_state(7);
                    scrambled_bits(bit_idx) = bitxor(byte_bits(bit_idx), scrambler_output);
                    
                    % Update: S(x) = x^7 + x^4 + 1
                    new_bit = bitxor(scrambler_state(7), scrambler_state(4));
                    scrambler_state = [new_bit, scrambler_state(1:6)];
                end
                
                scrambled_data(byte_idx) = bi2de(scrambled_bits, 'right-msb');
            end
            
            % Store seed in SERVICE byte 1 (bits 0-6)
            % SERVICE is first 2 bytes of scrambled_data
            scrambled_data(1) = bitand(scrambled_data(1), 128) + seed;
            
            % Assemble output: SIGNAL (unchanged) + scrambled DATA
            outputData = zeros(config.outputSize(2), 1, 'int8');
            
            % SIGNAL unchanged (3 bytes)
            outputData(1:3) = int8(int32(signal_field) - 128);
            
            % Scrambled DATA
            outputData(4:3+dataLength) = int8(int32(scrambled_data) - 128);
            
            % Write outputs
            pipeline_mex('write', pipeOutRate, rateLengthInput);
            pipeline_mex('write', pipeOutData, outputData);
            
            totalBytes = totalBytes + length(outputData);
            
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
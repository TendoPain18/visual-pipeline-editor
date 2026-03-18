function channel_encode(pipeInRate, pipeInData, pipeOutRate, pipeOutData)
% CHANNEL_ENCODE_FIXED - IEEE 802.11a Convolutional Encoder
%
% Input 1: rate_length (3 bytes: rate value + data length as uint16 LE)
% Input 2: SIGNAL + DATA (max 1515 bytes)
% Output 1: rate_encoded_length (3 bytes: rate + encoded length as uint16)
% Output 2: encoded data (SIGNAL@R=1/2 + DATA@variable rate)
%
% SIGNAL: always 24 bits -> 48 bits @ R=1/2 = 6 bytes
% DATA: variable bits -> variable encoded bits
%
% @BlockConfig
% name: ChannelEncode
% inputs: 2
% outputs: 2
% inputSize: [3, 1515]
% outputSize: [3, 3030]
% LTR: true
% startWithAll: true
% description: Convolutional encoder with variable output
% @EndBlockConfig

    config = parse_block_config();
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');

    try
        % Generator polynomials
        g0 = [1 0 1 1 0 1 1];
        g1 = [1 1 1 1 0 0 1];
        
        % RATE value to coding rate
        rateValueToCodingRate = containers.Map(...
            [13, 15, 5, 7, 9, 11, 1, 3], ...
            {'1/2', '1/2', '2/3', '2/3', '3/4', '3/4', '2/3', '3/4'});
        
        punctureMap = containers.Map(...
            {'1/2', '2/3', '3/4'}, ...
            {[], [1 1; 1 0], [1 1 0; 1 0 1]});
        
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
            
            % Get coding rate for DATA
            dataRateStr = rateValueToCodingRate(rateValue);
            puncture_pattern = punctureMap(dataRateStr);
            
            % Extract SIGNAL (first 3 bytes = 24 bits)
            signalBytes = dataBytes(1:3);
            signalBits = zeros(1, 24);
            for i = 1:3
                bits = de2bi(signalBytes(i), 8, 'right-msb');
                signalBits((i-1)*8 + 1 : i*8) = bits;
            end
            
            % Encode SIGNAL at R=1/2 (24 bits -> 48 bits)
            shiftReg = zeros(1, 7);
            signalEncoded = zeros(1, 48);
            for i = 1:24
                shiftReg = [signalBits(i), shiftReg(1:6)];
                outA = mod(sum(shiftReg .* g0), 2);
                outB = mod(sum(shiftReg .* g1), 2);
                signalEncoded((i-1)*2 + 1) = outA;
                signalEncoded((i-1)*2 + 2) = outB;
            end
            
            % Extract DATA (SERVICE + PSDU + TAIL+PAD)
            dataFieldBytes = dataBytes(4:3+dataLength);
            numDataBytes = length(dataFieldBytes);
            numDataBits = numDataBytes * 8;
            dataBits = zeros(1, numDataBits);
            for i = 1:numDataBytes
                bits = de2bi(dataFieldBytes(i), 8, 'right-msb');
                dataBits((i-1)*8 + 1 : i*8) = bits;
            end
            
            % Encode DATA with RATE-determined coding rate
            shiftReg = zeros(1, 7);
            
            if isempty(puncture_pattern)
                % R=1/2: no puncturing
                dataEncoded = zeros(1, numDataBits * 2);
                outIdx = 1;
                for i = 1:numDataBits
                    shiftReg = [dataBits(i), shiftReg(1:6)];
                    outA = mod(sum(shiftReg .* g0), 2);
                    outB = mod(sum(shiftReg .* g1), 2);
                    dataEncoded(outIdx) = outA;
                    dataEncoded(outIdx + 1) = outB;
                    outIdx = outIdx + 2;
                end
            else
                % R=2/3 or R=3/4: with puncturing
                [~, pCols] = size(puncture_pattern);
                maxEncodedBits = numDataBits * 2;
                dataEncoded = zeros(1, maxEncodedBits);
                outIdx = 1;
                punctureIdx = 0;
                
                for i = 1:numDataBits
                    shiftReg = [dataBits(i), shiftReg(1:6)];
                    outA = mod(sum(shiftReg .* g0), 2);
                    outB = mod(sum(shiftReg .* g1), 2);
                    
                    punctureCol = mod(punctureIdx, pCols) + 1;
                    
                    if puncture_pattern(1, punctureCol) == 1
                        dataEncoded(outIdx) = outA;
                        outIdx = outIdx + 1;
                    end
                    if puncture_pattern(2, punctureCol) == 1
                        dataEncoded(outIdx) = outB;
                        outIdx = outIdx + 1;
                    end
                    punctureIdx = punctureIdx + 1;
                end
                dataEncoded = dataEncoded(1:outIdx-1);
            end
            
            % Combine SIGNAL and DATA
            allEncodedBits = [signalEncoded, dataEncoded];
            totalEncodedBits = length(allEncodedBits);
            
            % Convert to bytes
            numOutBytes = ceil(totalEncodedBits / 8);
            if mod(totalEncodedBits, 8) ~= 0
                padBits = 8 - mod(totalEncodedBits, 8);
                allEncodedBits = [allEncodedBits, zeros(1, padBits)];
            end
            
            outputBytes = zeros(numOutBytes, 1, 'uint8');
            for i = 1:numOutBytes
                bitStart = (i-1) * 8 + 1;
                bitEnd = i * 8;
                outputBytes(i) = bi2de(allEncodedBits(bitStart:bitEnd), 'right-msb');
            end
            
            % Prepare outputs
            dataOutput = zeros(config.outputSize(2), 1, 'int8');
            copyLen = min(numOutBytes, config.outputSize(2));
            dataOutput(1:copyLen) = int8(int32(outputBytes(1:copyLen)) - 128);
            
            % Output 1: rate_encoded_length (3 bytes)
            % Byte 0: rate value
            % Bytes 1-2: encoded length in bytes (uint16, little-endian)
            rateEncodedLengthOutput = zeros(3, 1, 'int8');
            rateEncodedLengthOutput(1) = int8(int32(rateValue) - 128);
            rateEncodedLengthOutput(2) = int8(int32(bitand(numOutBytes, 255)) - 128);
            rateEncodedLengthOutput(3) = int8(int32(bitshift(numOutBytes, -8)) - 128);
            
            % Write outputs
            pipeline_mex('write', pipeOutRate, rateEncodedLengthOutput);
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
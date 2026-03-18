function channel_decode(pipeInSignal, pipeInData, pipeOutSignal, pipeOutData)
% CHANNEL_DECODE_SIGNAL_DATA - DEBUG VERSION
%
% Input 1: signal_bits (6 bytes, 48 bits encoded @ R=1/2)
% Input 2: data_bits + rate (rate in first byte, then encoded bits)
% Output 1: decoded signal_bits (3 bytes, 24 bits)
% Output 2: decoded data_bits (NO rate prepended)
%
% @BlockConfig
% name: ChannelDecode
% inputs: 2
% outputs: 2
% inputSize: [6, 3025]
% outputSize: [3, 1512]
% LTR: false
% startWithAll: true
% description: DEBUG - Viterbi decoder with extensive logging
% @EndBlockConfig

    config = parse_block_config();
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');

    try
        % Generator polynomials
        g0 = [1 0 1 1 0 1 1];
        g1 = [1 1 1 1 0 0 1];
        numStates = 64;
        
        % Build trellis
        nextState = zeros(numStates, 2);
        output = zeros(numStates, 2, 2);
        for state = 0:numStates-1
            stateBits = de2bi(state, 6, 'right-msb');
            for inputBit = 0:1
                newReg = [inputBit, stateBits];
                outA = mod(sum(newReg .* g0), 2);
                outB = mod(sum(newReg .* g1), 2);
                nextSt = bi2de(newReg(1:6), 'right-msb');
                nextState(state + 1, inputBit + 1) = nextSt;
                output(state + 1, inputBit + 1, :) = [outA, outB];
            end
        end
        
        % RATE value to coding rate
        rateValueToCodingRate = containers.Map(...
            [13, 15, 5, 7, 9, 11, 1, 3], ...
            {'1/2', '1/2', '2/3', '2/3', '3/4', '3/4', '2/3', '3/4'});
        
        depunctureMap = containers.Map(...
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
            
            % ===== STEP 1: Wait for SIGNAL =====
            try
                signalInput = pipeline_mex('read', pipeInSignal, config.inputSize(1));
            catch ME
                fprintf('ERROR reading SIGNAL: %s\n', ME.message);
                rethrow(ME);
            end

            signalBytes = uint8(int32(signalInput) + 128);

            
            % Convert to bits (48 bits)
            signalBits = zeros(1, 48);
            for i = 1:6
                bits = de2bi(signalBytes(i), 8, 'right-msb');
                signalBits((i-1)*8 + 1 : i*8) = bits;
            end

            
            % Decode SIGNAL (always R=1/2, 48 bits -> 24 bits)
            decodedSignal = viterbi_decode(signalBits, [], numStates, nextState, output);
            
            % Convert to bytes (24 bits = 3 bytes)
            signalOutBytes = zeros(3, 1, 'uint8');
            for i = 1:3
                bitStart = (i-1) * 8 + 1;
                signalOutBytes(i) = bi2de(decodedSignal(bitStart:bitStart+7), 'right-msb');
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
            
            % Get coding rate and depuncture
            dataRateStr = rateValueToCodingRate(rateValue);
            depuncture_pattern = depunctureMap(dataRateStr);
                        
            % Depuncture if needed
            if ~isempty(depuncture_pattern)
                dataBits = depuncture_bits(dataBits, depuncture_pattern);
            end
            
            % Decode DATA
            decodedData = viterbi_decode(dataBits, [], numStates, nextState, output);
            
            % Convert to bytes
            numOutBits = length(decodedData);
            numOutBytes = floor(numOutBits / 8);
            dataOutBytes = zeros(numOutBytes, 1, 'uint8');
            for i = 1:numOutBytes
                bitStart = (i-1) * 8 + 1;
                bitEnd = i * 8;
                dataOutBytes(i) = bi2de(decodedData(bitStart:bitEnd), 'right-msb');
            end
            
            % Send DATA output (NO rate prepended)
            dataOutput = zeros(config.outputSize(2), 1, 'int8');
            copyLen = min(numOutBytes, config.outputSize(2));
            dataOutput(1:copyLen) = int8(int32(dataOutBytes(1:copyLen)) - 128);
            
            try
                pipeline_mex('write', pipeOutData, dataOutput);
            catch ME
                fprintf('ERROR writing DATA: %s\n', ME.message);
                rethrow(ME);
            end
                        
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
        fprintf('\nCHANNEL DECODE FATAL ERROR in frame %d\n', frameCount);
        fprintf('Message: %s\n', ME.message);
        send_protocol_message('BLOCK_ERROR', config.blockId, config.name, ME.message);
        rethrow(ME);
    end
end

function depuncturedBits = depuncture_bits(puncturedBits, pattern)
    [~, pCols] = size(pattern);
    numInputPairs = floor(length(puncturedBits) / sum(sum(pattern)));
    depuncturedBits = zeros(1, numInputPairs * 2 * pCols);
    outIdx = 1;
    inIdx = 1;
    for pair = 1:numInputPairs
        for col = 1:pCols
            if pattern(1, col) == 1
                depuncturedBits(outIdx) = puncturedBits(inIdx);
                inIdx = inIdx + 1;
            else
                depuncturedBits(outIdx) = -1;
            end
            outIdx = outIdx + 1;
            if pattern(2, col) == 1
                depuncturedBits(outIdx) = puncturedBits(inIdx);
                inIdx = inIdx + 1;
            else
                depuncturedBits(outIdx) = -1;
            end
            outIdx = outIdx + 1;
        end
    end
    depuncturedBits = depuncturedBits(1:outIdx-1);
end

function decodedBits = viterbi_decode(receivedBits, ~, numStates, nextState, output)
    numReceivedPairs = floor(length(receivedBits) / 2);
    pathMetric = inf(numStates, 1);
    pathMetric(1) = 0;
    survivors = zeros(numStates, numReceivedPairs);
    
    for t = 1:numReceivedPairs
        recvA = receivedBits((t-1)*2 + 1);
        recvB = receivedBits((t-1)*2 + 2);
        newMetric = inf(numStates, 1);
        
        for state = 0:numStates-1
            for inputBit = 0:1
                prevState = state;
                expA = output(prevState + 1, inputBit + 1, 1);
                expB = output(prevState + 1, inputBit + 1, 2);
                branchMetric = 0;
                
                if recvA ~= -1
                    branchMetric = branchMetric + (recvA ~= expA);
                end
                if recvB ~= -1
                    branchMetric = branchMetric + (recvB ~= expB);
                end
                
                nextSt = nextState(prevState + 1, inputBit + 1);
                candidateMetric = pathMetric(prevState + 1) + branchMetric;
                
                if candidateMetric < newMetric(nextSt + 1)
                    newMetric(nextSt + 1) = candidateMetric;
                    survivors(nextSt + 1, t) = prevState * 2 + inputBit;
                end
            end
        end
        pathMetric = newMetric;
    end
    
    [~, finalState] = min(pathMetric);
    finalState = finalState - 1;
    decodedBits = zeros(1, numReceivedPairs);
    currentState = finalState;
    
    for t = numReceivedPairs:-1:1
        stateAndInput = survivors(currentState + 1, t);
        inputBit = mod(stateAndInput, 2);
        currentState = floor(stateAndInput / 2);
        decodedBits(t) = inputBit;
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
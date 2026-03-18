function channel_encode_middleman(pipeInRate, pipeInData, pipeInFeedback, pipeOutSignal, pipeOutData)
% CHANNEL_ENCODE_MIDDLEMAN - Splits channel encoder output for channel_decode
%
% Sits between channel_encode and channel_decode.
% Discards rate_encoded_length, splits the encoded data stream into 
% SIGNAL (first 6 bytes, 48 bits) and DATA (remaining bytes).
% Waits for rate_length feedback from ppdu_decapsulate, then sends 
% rate+data to channel_decode.
%
% Input 1:  pipeInRate     - rate_encoded_length from channel_encode (3 bytes, DISCARDED)
% Input 2:  pipeInData     - full encoded packet from channel_encode (max 3030 bytes)
%                            format: SIGNAL(6 bytes, 48 bits @ R=1/2) + DATA(variable)
% Input 3:  pipeInFeedback - rate_length feedback from ppdu_decapsulate (3 bytes: rate + length uint16)
%
% Output 1: pipeOutSignal  - first 6 bytes (SIGNAL field, encoded @ R=1/2) -> channel_decode input 1
% Output 2: pipeOutData    - rate (1 byte) + remaining encoded DATA -> channel_decode input 2
%
% Sequence per frame:
%   1. Read and discard rate_encoded_length (pipeInRate)
%   2. Read full encoded packet (pipeInData)
%   3. Send SIGNAL (6 bytes) to channel_decode
%   4. Wait for rate_length feedback (3 bytes) from ppdu_decapsulate  <-- gate
%   5. Extract rate from feedback and send rate + encoded DATA to channel_decode
%   6. Repeat
%
% @BlockConfig
% name: ChannelEncodeMiddleman
% inputs: 3
% outputs: 2
% inputSize: [3, 3030, 3]
% outputSize: [6, 3025]
% LTR: false
% startWithAll: true
% description: Splits channel encoder stream - sends SIGNAL first, waits for ppdu_decap feedback, then sends rate+DATA
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

            % ===== STEP 1: Read and discard rate_encoded_length =====
            try
                rateInput = pipeline_mex('read', pipeInRate, config.inputSize(1));
            catch ME
                fprintf('ERROR reading rate_encoded_length: %s\n', ME.message);
                rethrow(ME);
            end

            % ===== STEP 2: Read full encoded packet from channel_encode =====
            try
                dataInput = pipeline_mex('read', pipeInData, config.inputSize(2));
            catch ME
                fprintf('ERROR reading full packet: %s\n', ME.message);
                rethrow(ME);
            end

            % ===== STEP 3: Split SIGNAL (bytes 1-6) from DATA (bytes 7-end) =====
            signalOutput = dataInput(1:6);
            dataField    = dataInput(7:end);

            % ===== STEP 4: Send SIGNAL to channel_decode =====
            try
                pipeline_mex('write', pipeOutSignal, signalOutput);
            catch ME
                fprintf('ERROR writing SIGNAL: %s\n', ME.message);
                rethrow(ME);
            end

            % ===== STEP 5: Wait for rate_length feedback from ppdu_decapsulate =====
            try
                feedbackInput = pipeline_mex('read', pipeInFeedback, config.inputSize(3));
                rateValue = uint8(int32(feedbackInput(1)) + 128);
            catch ME
                fprintf('ERROR reading feedback: %s\n', ME.message);
                rethrow(ME);
            end

            % ===== STEP 6: Send rate + encoded DATA to channel_decode =====
            % Format: rate (1 byte) + encoded DATA
            dataOutput = zeros(config.outputSize(2), 1, 'int8');
            dataOutput(1) = int8(int32(rateValue) - 128);
            copyLen = min(length(dataField), config.outputSize(2) - 1);
            dataOutput(2:copyLen+1) = dataField(1:copyLen);

            try
                pipeline_mex('write', pipeOutData, dataOutput);
            catch ME
                fprintf('ERROR writing DATA: %s\n', ME.message);
                rethrow(ME);
            end

            totalBytes = totalBytes + copyLen;
            
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
        fprintf('\nCHANNEL ENCODE MIDDLEMAN FATAL ERROR in frame %d\n', frameCount);
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
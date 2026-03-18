function ppdu_middleman(pipeInRate, pipeInData, pipeInFeedback, pipeOutSignal, pipeOutData)
% PPDU_MIDDLEMAN - Splits PPDU encapsulator output for decapsulator
%
% Sits between ppdu_encapsulate and ppdu_decapsulate.
% Discards rate/length metadata, splits the data stream into
% SIGNAL (first 3 bytes) and DATA (remaining 1512 bytes),
% and uses feedback from ppdu_decapsulate as a gate before
% sending the DATA portion.
%
% Input 1:  pipeInRate     - rate_length from ppdu_encapsulate (3 bytes, DISCARDED)
% Input 2:  pipeInData     - full packet from ppdu_encapsulate (1515 bytes)
%                            format: SIGNAL(3) + SERVICE(2) + PSDU(1504) + TAIL+PAD(6)
% Input 3:  pipeInFeedback - feedback from ppdu_decapsulate (3 bytes, used as trigger only)
%
% Output 1: pipeOutSignal  - first 3 bytes (SIGNAL field) -> ppdu_decapsulate input 1
% Output 2: pipeOutData    - remaining 1512 bytes (SERVICE+PSDU+TAIL+PAD) -> ppdu_decapsulate input 2
%
% Sequence per frame:
%   1. Read and discard rate/length (pipeInRate)
%   2. Read full 1515-byte packet (pipeInData)
%   3. Send SIGNAL (3 bytes) to ppdu_decapsulate
%   4. Wait for feedback (3 bytes) from ppdu_decapsulate  <-- gate
%   5. Send remaining DATA (1512 bytes) to ppdu_decapsulate
%   6. Repeat
%
% @BlockConfig
% name: PpduMiddleman
% inputs: 3
% outputs: 2
% inputSize: [3, 1515, 3]
% outputSize: [3, 1512]
% LTR: false
% startWithAll: true
% description: Splits PPDU stream - sends SIGNAL first, waits for feedback, then sends DATA
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

            % ===== STEP 1: Read and discard rate/length =====
            try
                rateInput = pipeline_mex('read', pipeInRate, config.inputSize(1));
            catch ME
                fprintf('ERROR reading rate/length: %s\n', ME.message);
                rethrow(ME);
            end

            % ===== STEP 2: Read full packet from encapsulator =====
            try
                dataInput = pipeline_mex('read', pipeInData, config.inputSize(2));
            catch ME
                fprintf('ERROR reading full packet: %s\n', ME.message);
                rethrow(ME);
            end

            % ===== STEP 3: Split SIGNAL (bytes 1-3) from DATA (bytes 4-1515) =====
            signalOutput = dataInput(1:3);
            dataOutput   = dataInput(4:end);

            % Pad or trim dataOutput to exactly outputSize(2) bytes just in case
            if length(dataOutput) < config.outputSize(2)
                padded = zeros(config.outputSize(2), 1, 'int8');
                padded(1:length(dataOutput)) = dataOutput;
                dataOutput = padded;
            elseif length(dataOutput) > config.outputSize(2)
                dataOutput = dataOutput(1:config.outputSize(2));
            end

            % Log SIGNAL bytes for debugging
            signalBytes = uint8(int32(signalOutput) + 128);

            % ===== STEP 4: Send SIGNAL to ppdu_decapsulate =====
            try
                pipeline_mex('write', pipeOutSignal, signalOutput);
            catch ME
                fprintf('ERROR writing SIGNAL: %s\n', ME.message);
                rethrow(ME);
            end

            % ===== STEP 5: Wait for feedback from ppdu_decapsulate =====
            try
                feedbackInput = pipeline_mex('read', pipeInFeedback, config.inputSize(3));
            catch ME
                fprintf('ERROR reading feedback: %s\n', ME.message);
                rethrow(ME);
            end

            % ===== STEP 6: Send remaining DATA to ppdu_decapsulate =====
            try
                pipeline_mex('write', pipeOutData, dataOutput);
            catch ME
                fprintf('ERROR writing DATA: %s\n', ME.message);
                rethrow(ME);
            end


            totalBytes = totalBytes + length(dataOutput);
            
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
        fprintf('\nPPDU MIDDLEMAN FATAL ERROR in frame %d\n', frameCount);
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
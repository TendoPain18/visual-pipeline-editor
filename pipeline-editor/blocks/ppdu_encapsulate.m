function ppdu_encapsulate(pipeIn, pipeOutRate, pipeOutData)
% PPDU_ENCAPSULATE_FIXED - IEEE 802.11a PPDU with variable padding
%
% Input:  PSDU (1504 bytes = 1500 payload + 4 CRC)
% Output 1: rate_length (3 bytes: rate value + data length as uint16 LE)
% Output 2: DATA field (SIGNAL[3] + SERVICE[2] + PSDU[1504] + TAIL[6bits] + PAD[6-42bits])
%
% @BlockConfig
% name: PpduEncapsulate
% inputs: 1
% outputs: 2
% inputSize: 1504
% outputSize: [3, 1515]
% LTR: true
% startWithAll: true
% dataRate: 12
% description: PPDU encapsulation with correct variable padding
% @EndBlockConfig

    config = parse_block_config();
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');

    try
        % Rate parameters per IEEE 802.11a
        rateParams = containers.Map([6, 9, 12, 18, 24, 36, 48, 54], ...
            {struct('R1_R4', [1,1,0,1], 'value', 13, 'NDBPS', 24), ...
             struct('R1_R4', [1,1,1,1], 'value', 15, 'NDBPS', 36), ...
             struct('R1_R4', [1,0,1,0], 'value', 5,  'NDBPS', 48), ...
             struct('R1_R4', [1,1,1,0], 'value', 7,  'NDBPS', 72), ...
             struct('R1_R4', [1,0,0,1], 'value', 9,  'NDBPS', 96), ...
             struct('R1_R4', [1,1,0,1], 'value', 11, 'NDBPS', 144), ...
             struct('R1_R4', [1,0,0,0], 'value', 1,  'NDBPS', 192), ...
             struct('R1_R4', [1,1,0,0], 'value', 3,  'NDBPS', 216)});
        
        params = rateParams(config.dataRate);
        rateBits = params.R1_R4;
        
        % Calculate padding
        L_FRAMING = 1500 * 8;
        L_CRC = L_FRAMING + 32;
        L_PPDU = 16 + L_CRC + 6;  % SERVICE + PSDU+CRC + TAIL
        N_DATA = L_PPDU;
        N_DBPS = params.NDBPS;
        N_SYM = ceil(N_DATA / N_DBPS);
        N_DATApadded = N_SYM * N_DBPS;
        N_PAD = N_DATApadded - N_DATA;
        
        % Calculate actual DATA size in bytes
        % DATA = SERVICE(16 bits) + PSDU+CRC(12032 bits) + TAIL(6 bits) + PAD(N_PAD bits)
        totalDataBits = 16 + 12032 + 6 + N_PAD;
        dataLengthBytes = ceil(totalDataBits / 8);
        
        send_protocol_message('BLOCK_READY', config.blockId, config.name, '');
        
        frameCount = 0;
        totalBytes = 0;
        startTime = tic;
        lastTime = 0;
        lastBytes = 0;
        
        while true
            % Read PSDU (1504 bytes = 1500 payload + 4 CRC)
            psdu = pipeline_mex('read', pipeIn, config.inputSize);
            psduBytes = uint8(int32(psdu) + 128);
            LENGTH = length(psduBytes);
            
            frameCount = frameCount + 1;
            
            % Build SIGNAL field (3 bytes, 24 bits)
            rate_field = rateBits(1) + rateBits(2)*2 + rateBits(3)*4 + rateBits(4)*8;
            
            % Calculate parity
            signal_bits = [rateBits, 0];  % R1-R4 + reserved bit
            for i = 0:11
                signal_bits = [signal_bits, bitget(LENGTH, i+1)];
            end
            parity = mod(sum(signal_bits), 2);
            
            % Pack SIGNAL into 3 bytes
            signal_byte1 = uint8(rate_field + ...
                          bitshift(bitand(LENGTH, 1), 5) + ...
                          bitshift(bitand(bitshift(LENGTH, -1), 1), 6) + ...
                          bitshift(bitand(bitshift(LENGTH, -2), 1), 7));
            
            signal_byte2 = uint8(bitand(bitshift(LENGTH, -3), 255));
            signal_byte3 = uint8(bitand(bitshift(LENGTH, -11), 1) + bitshift(parity, 1));
            
            % Build SERVICE field (2 bytes)
            service_byte1 = uint8(0);
            service_byte2 = uint8(0);
            
            % Build TAIL (6 zero bits) + PAD (N_PAD bits)
            tailPadBits = [zeros(1, 6), zeros(1, N_PAD)];
            tailPadBytes = zeros(ceil((6 + N_PAD) / 8), 1, 'uint8');
            for i = 1:length(tailPadBytes)
                bitStart = (i-1) * 8 + 1;
                bitEnd = min(i * 8, length(tailPadBits));
                if bitEnd >= bitStart
                    tailPadBytes(i) = bi2de(tailPadBits(bitStart:bitEnd), 'right-msb');
                end
            end
            
            % Assemble DATA output: SIGNAL + SERVICE + PSDU + TAIL+PAD
            dataOutput = zeros(config.outputSize(2), 1, 'int8');
            idx = 1;
            
            % SIGNAL (3 bytes)
            dataOutput(idx:idx+2) = int8(int32([signal_byte1; signal_byte2; signal_byte3]) - 128);
            idx = idx + 3;
            
            % SERVICE (2 bytes)
            dataOutput(idx:idx+1) = int8(int32([service_byte1; service_byte2]) - 128);
            idx = idx + 2;
            
            % PSDU (1504 bytes)
            dataOutput(idx:idx+LENGTH-1) = int8(int32(psduBytes) - 128);
            idx = idx + LENGTH;
            
            % TAIL + PAD
            dataOutput(idx:idx+length(tailPadBytes)-1) = int8(int32(tailPadBytes) - 128);
            
            % Output 1: rate_length (3 bytes)
            % Byte 0: rate value
            % Byte 1-2: data length in bytes as uint16 LE (little-endian)
            rateLengthOutput = zeros(3, 1, 'int8');
            rateLengthOutput(1) = int8(int32(params.value) - 128);
            rateLengthOutput(2) = int8(int32(bitand(dataLengthBytes, 255)) - 128);  % Low byte
            rateLengthOutput(3) = int8(int32(bitshift(dataLengthBytes, -8)) - 128);  % High byte
            
            % Write outputs
            pipeline_mex('write', pipeOutRate, rateLengthOutput);
            pipeline_mex('write', pipeOutData, dataOutput);
            
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
function qam_mapper(pipeInRate, pipeInData, pipeOutOfdmSymbols)
% QAM_MAPPER_OFDM - Maps bits to QAM symbols and arranges into OFDM symbols
%
% Input 1: rate_encoded_length (3 bytes: rate + encoded length uint16)
% Input 2: interleaved data (SIGNAL + DATA, max 3030 bytes)
% Output 1: OFDM symbols (64 complex symbols per OFDM symbol, 4 bytes per subcarrier)
%
% OFDM Symbol Structure (64 subcarriers, IEEE 802.11a):
%   Subcarriers -26 to -22, -20 to -8, -6 to -1, 1 to 6, 8 to 20, 22 to 26 (48 data)
%   Subcarriers -21, -7, 7, 21 (4 pilots)
%   Subcarrier 0 (DC, null)
%   Remaining (11 null subcarriers at edges)
%
% Each OFDM symbol output: 256 bytes (64 subcarriers × 4 bytes)
%   Per subcarrier: I[2 bytes] + Q[2 bytes] (int16, little-endian, scaled by 32767)
%
% SIGNAL field: 1 OFDM symbol (48 BPSK symbols)
% DATA field: N_SYM OFDM symbols based on rate
%
% @BlockConfig
% name: QamMapperOfdm
% inputs: 2
% outputs: 1
% inputSize: [3, 3030]
% outputSize: 256
% LTR: true
% startWithAll: true
% description: QAM mapper with OFDM symbol structuring (64 subcarriers per symbol)
% @EndBlockConfig

    config = parse_block_config();
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');

    try
        % RATE to modulation parameters
        rateParams = containers.Map(...
            [13, 15, 5, 7, 9, 11, 1, 3], ...
            {struct('NBPSC', 1, 'NCBPS', 48, 'NDBPS', 24, 'mod', 'BPSK'), ...   % 6 Mbps
             struct('NBPSC', 1, 'NCBPS', 48, 'NDBPS', 36, 'mod', 'BPSK'), ...   % 9 Mbps
             struct('NBPSC', 2, 'NCBPS', 96, 'NDBPS', 48, 'mod', 'QPSK'), ...   % 12 Mbps
             struct('NBPSC', 2, 'NCBPS', 96, 'NDBPS', 72, 'mod', 'QPSK'), ...   % 18 Mbps
             struct('NBPSC', 4, 'NCBPS', 192, 'NDBPS', 96, 'mod', '16QAM'), ... % 24 Mbps
             struct('NBPSC', 4, 'NCBPS', 192, 'NDBPS', 144, 'mod', '16QAM'), ... % 36 Mbps
             struct('NBPSC', 6, 'NCBPS', 288, 'NDBPS', 192, 'mod', '64QAM'), ... % 48 Mbps
             struct('NBPSC', 6, 'NCBPS', 288, 'NDBPS', 216, 'mod', '64QAM')});  % 54 Mbps
        
        % Pre-compute constellation maps (IEEE 802.11a Gray-coded)
        bpsk_map = [-1, 1];
        qpsk_map = (1/sqrt(2)) * [-1-1i, -1+1i, 1-1i, 1+1i];
        qam16_map = get_16qam_constellation();
        qam64_map = get_64qam_constellation();
        
        % Pilot sequence (127-element, cyclic)
        pilot_seq = [1,1,1,1, -1,-1,-1,1, -1,-1,-1,-1, 1,1,-1,1, -1,-1,1,1, -1,1,1,-1, ...
                     1,1,1,1, 1,1,-1,1, 1,1,-1,1, 1,-1,-1,1, 1,1,-1,1, -1,-1,-1,1, -1,1,-1,-1, ...
                     1,-1,-1,1, 1,1,1,1, -1,-1,1,1, -1,-1,1,-1, 1,-1,1,1, -1,-1,-1,1, 1,-1,-1,-1, ...
                     -1,1,-1,-1, 1,-1,1,1, 1,1,-1,1, -1,1,-1,1, -1,-1,-1,-1, -1,1,-1,1, 1,-1,1,-1, ...
                     1,1,1,-1, -1,1,-1,-1, -1,1,1,1, -1,-1,-1,-1, -1,-1,-1];
        
        % Subcarrier mapping: data symbol index -> OFDM subcarrier index
        data_subcarrier_map = [-26:-22, -20:-8, -6:-1, 1:6, 8:20, 22:26];
        
        % Pilot subcarriers at -21, -7, 7, 21
        pilot_subcarriers = [-21, -7, 7, 21];
        
        send_protocol_message('BLOCK_READY', config.blockId, config.name, '');
        
        frameCount = 0;
        totalOfdmSymbols = 0;
        startTime = tic;
        lastTime = 0;
        lastSymbols = 0;
        
        while true
            frameCount = frameCount + 1;
            
            % Read rate_encoded_length
            rateEncodedLengthInput = pipeline_mex('read', pipeInRate, config.inputSize(1));
            rateValue = uint8(int32(rateEncodedLengthInput(1)) + 128);
            encodedLengthLow = uint8(int32(rateEncodedLengthInput(2)) + 128);
            encodedLengthHigh = uint8(int32(rateEncodedLengthInput(3)) + 128);
            encodedLength = uint16(encodedLengthLow) + bitshift(uint16(encodedLengthHigh), 8);
            
            % Read interleaved data
            inputData = pipeline_mex('read', pipeInData, config.inputSize(2));
            dataBytes = uint8(int32(inputData(1:encodedLength)) + 128);
            
            % Convert to bits
            numBits = encodedLength * 8;
            inputBits = zeros(1, numBits);
            for i = 1:encodedLength
                bits = de2bi(dataBytes(i), 8, 'right-msb');
                inputBits((i-1)*8 + 1 : i*8) = bits;
            end
            
            % DEBUG: Print first SIGNAL byte on first frame
            if frameCount <= 3
                fprintf('\n========================================\n');
                fprintf('MAPPER Frame %d\n', frameCount);
                fprintf('========================================\n');
                fprintf('First SIGNAL byte: 0x%02X\n', dataBytes(1));
                fprintf('RATE value: %d\n', rateValue);
                fprintf('Encoded length: %d bytes (%d bits)\n', encodedLength, numBits);
            end
            
            % ===== SIGNAL field (1 OFDM symbol, always BPSK) =====
            signalBits = inputBits(1:48);
            ofdmSymbol = create_ofdm_symbol(signalBits, bpsk_map, qpsk_map, qam16_map, qam64_map, ...
                                           'BPSK', 1, data_subcarrier_map, pilot_subcarriers, pilot_seq, 0);
            
            % Send SIGNAL OFDM symbol
            pipeline_mex('write', pipeOutOfdmSymbols, ofdmSymbol);
            totalOfdmSymbols = totalOfdmSymbols + 1;
            
            if frameCount <= 3
                fprintf('Sent SIGNAL OFDM symbol (1 symbol)\n');
            end
            
            % ===== DATA field based on rate =====
            dataBits = inputBits(49:end);
            params = rateParams(rateValue);
            NBPSC = params.NBPSC;
            NCBPS = params.NCBPS;
            N_DBPS = params.NDBPS;
            modType = params.mod;
            
            % Calculate N_SYM - MUST match demapper calculation
            % This is based on the ACTUAL payload size (1500 bytes)
            L_FRAMING = 1500 * 8;     % Payload only
            L_CRC = L_FRAMING + 32;   % Add CRC bits
            L_PPDU = 16 + L_CRC + 6;  % SERVICE + data + TAIL
            N_DATA = L_PPDU;
            N_SYM = ceil(N_DATA / N_DBPS);
            
            % Total bits needed for N_SYM OFDM symbols
            totalBitsNeeded = N_SYM * NCBPS;
            
            if frameCount <= 3
                fprintf('\nDATA field calculation:\n');
                fprintf('  Modulation: %s\n', modType);
                fprintf('  N_DBPS: %d bits/symbol\n', N_DBPS);
                fprintf('  NCBPS: %d bits/OFDM symbol\n', NCBPS);
                fprintf('  NBPSC: %d bits/subcarrier\n', NBPSC);
                fprintf('  L_FRAMING: %d bits\n', L_FRAMING);
                fprintf('  L_CRC: %d bits\n', L_CRC);
                fprintf('  L_PPDU: %d bits\n', L_PPDU);
                fprintf('  N_DATA: %d bits\n', N_DATA);
                fprintf('  N_SYM = ceil(%d / %d) = %d\n', N_DATA, N_DBPS, N_SYM);
                fprintf('  Total bits needed: %d x %d = %d bits\n', N_SYM, NCBPS, totalBitsNeeded);
                fprintf('  Available DATA bits: %d\n', length(dataBits));
            end
            
            % Pad data bits if needed
            if length(dataBits) < totalBitsNeeded
                padBits = totalBitsNeeded - length(dataBits);
                dataBits = [dataBits, zeros(1, padBits)];
                if frameCount <= 3
                    fprintf('  Added %d padding bits\n', padBits);
                end
            elseif length(dataBits) > totalBitsNeeded
                % Truncate if we have more bits than needed
                dataBits = dataBits(1:totalBitsNeeded);
                if frameCount <= 3
                    fprintf('  Truncated to %d bits\n', totalBitsNeeded);
                end
            end
            
            if frameCount <= 3
                fprintf('\nSending %d DATA OFDM symbols...\n', N_SYM);
            end
            
            % Process each OFDM symbol
            for ofdmIdx = 1:N_SYM
                startIdx = (ofdmIdx - 1) * NCBPS + 1;
                endIdx = ofdmIdx * NCBPS;
                symbolDataBits = dataBits(startIdx:endIdx);
                
                % Pilot index (SIGNAL uses p0, DATA starts at p1)
                pilotIdx = ofdmIdx;
                
                ofdmSymbol = create_ofdm_symbol(symbolDataBits, bpsk_map, qpsk_map, qam16_map, qam64_map, ...
                                               modType, NBPSC, data_subcarrier_map, pilot_subcarriers, ...
                                               pilot_seq, pilotIdx);
                
                pipeline_mex('write', pipeOutOfdmSymbols, ofdmSymbol);
                totalOfdmSymbols = totalOfdmSymbols + 1;
                
                % Progress for first frame
                if frameCount == 1 && mod(ofdmIdx, 50) == 0
                    fprintf('  Sent OFDM symbol %d/%d\n', ofdmIdx, N_SYM);
                end
            end
            
            if frameCount <= 3
                fprintf('Finished sending %d DATA OFDM symbols\n', N_SYM);
                fprintf('Total OFDM symbols this frame: %d (1 SIGNAL + %d DATA)\n', 1 + N_SYM, N_SYM);
                fprintf('========================================\n\n');
            end
            
            % Metrics
            currentTime = toc(startTime);
            elapsed = currentTime - lastTime;
            if elapsed > 0
                instantRate = (totalOfdmSymbols - lastSymbols) / elapsed;
            else
                instantRate = 0;
            end
            lastTime = currentTime;
            lastSymbols = totalOfdmSymbols;
            
            metrics = struct();
            metrics.frames = frameCount;
            metrics.gbps = instantRate / 1e6;
            send_protocol_message('BLOCK_METRICS', config.blockId, config.name, metrics);
        end
        
    catch ME
        fprintf('\nMAPPER FATAL ERROR in frame %d\n', frameCount);
        fprintf('Message: %s\n', ME.message);
        fprintf('Stack trace:\n');
        for i = 1:length(ME.stack)
            fprintf('  %s (line %d)\n', ME.stack(i).name, ME.stack(i).line);
        end
        send_protocol_message('BLOCK_ERROR', config.blockId, config.name, ME.message);
        rethrow(ME);
    end
end

function ofdmSymbol = create_ofdm_symbol(dataBits, bpsk_map, qpsk_map, qam16_map, qam64_map, ...
                                         modType, NBPSC, data_subcarrier_map, pilot_subcarriers, ...
                                         pilot_seq, pilotIdx)
    % Create 64-subcarrier OFDM symbol
    % Initialize all subcarriers to zero (null)
    subcarriers = zeros(1, 64);
    
    % Map data bits to QAM symbols
    numDataSymbols = length(dataBits) / NBPSC;
    dataSymbols = zeros(1, 48);
    
    for sym = 1:numDataSymbols
        startIdx = (sym - 1) * NBPSC + 1;
        endIdx = sym * NBPSC;
        symbolBits = dataBits(startIdx:endIdx);
        
        symbolIndex = bi2de(symbolBits, 'left-msb') + 1;
        
        switch modType
            case 'BPSK'
                dataSymbols(sym) = bpsk_map(symbolIndex);
            case 'QPSK'
                dataSymbols(sym) = qpsk_map(symbolIndex);
            case '16QAM'
                dataSymbols(sym) = qam16_map(symbolIndex);
            case '64QAM'
                dataSymbols(sym) = qam64_map(symbolIndex);
        end
    end
    
    % Place data symbols in their subcarrier positions
    for i = 1:48
        sc_idx = data_subcarrier_map(i);
        matlab_idx = sc_idx + 33;
        subcarriers(matlab_idx) = dataSymbols(i);
    end
    
    % Add pilot symbols
    p_n = pilot_seq(mod(pilotIdx, 127) + 1);
    
    for i = 1:4
        sc_idx = pilot_subcarriers(i);
        matlab_idx = sc_idx + 33;
        subcarriers(matlab_idx) = p_n;
    end
    
    % Convert complex symbols to packed int8 format
    ofdmSymbol = zeros(256, 1, 'int8');
    
    for i = 1:64
        symbol = subcarriers(i);
        
        I_val = int16(round(real(symbol) * 32767));
        Q_val = int16(round(imag(symbol) * 32767));
        
        I_bytes = typecast(I_val, 'uint8');
        Q_bytes = typecast(Q_val, 'uint8');
        
        baseIdx = (i - 1) * 4;
        ofdmSymbol(baseIdx + 1) = int8(int16(I_bytes(1)) - 128);
        ofdmSymbol(baseIdx + 2) = int8(int16(I_bytes(2)) - 128);
        ofdmSymbol(baseIdx + 3) = int8(int16(Q_bytes(1)) - 128);
        ofdmSymbol(baseIdx + 4) = int8(int16(Q_bytes(2)) - 128);
    end
end

function map = get_16qam_constellation()
    k = 1/sqrt(10);
    map = zeros(1, 16);
    for b0 = 0:1
        for b1 = 0:1
            for b2 = 0:1
                for b3 = 0:1
                    idx = b0*8 + b1*4 + b2*2 + b3 + 1;
                    b0b1 = b0*2 + b1;
                    switch b0b1
                        case 0, I = -3;
                        case 1, I = -1;
                        case 3, I = 1;
                        case 2, I = 3;
                    end
                    b2b3 = b2*2 + b3;
                    switch b2b3
                        case 0, Q = -3;
                        case 1, Q = -1;
                        case 3, Q = 1;
                        case 2, Q = 3;
                    end
                    map(idx) = k * (I + 1i * Q);
                end
            end
        end
    end
end

function map = get_64qam_constellation()
    k = 1/sqrt(42);
    map = zeros(1, 64);
    for b0 = 0:1
        for b1 = 0:1
            for b2 = 0:1
                for b3 = 0:1
                    for b4 = 0:1
                        for b5 = 0:1
                            idx = b0*32 + b1*16 + b2*8 + b3*4 + b4*2 + b5 + 1;
                            b0b1b2 = b0*4 + b1*2 + b2;
                            switch b0b1b2
                                case 0, I = -7;
                                case 1, I = -5;
                                case 3, I = -3;
                                case 2, I = -1;
                                case 6, I = 1;
                                case 7, I = 3;
                                case 5, I = 5;
                                case 4, I = 7;
                            end
                            b3b4b5 = b3*4 + b4*2 + b5;
                            switch b3b4b5
                                case 0, Q = -7;
                                case 1, Q = -5;
                                case 3, Q = -3;
                                case 2, Q = -1;
                                case 6, Q = 1;
                                case 7, Q = 3;
                                case 5, Q = 5;
                                case 4, Q = 7;
                            end
                            map(idx) = k * (I + 1i * Q);
                        end
                    end
                end
            end
        end
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
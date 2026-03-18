function qam_demapper_ofdm(pipeInOfdmSymbols, pipeInFeedback, pipeOutSignal, pipeOutData, pipeOutScatter)
% QAM_DEMAPPER_OFDM - Demaps OFDM symbols to bits with feedback-driven operation
%
% Input 1: OFDM symbols (64 complex symbols per OFDM symbol, 256 bytes total)
% Input 2: rate_length feedback from ppdu_decapsulate (3 bytes: rate + length uint16)
% Output 1: SIGNAL bits (6 bytes = 48 bits demapped from first OFDM symbol)
% Output 2: rate + DATA bits (rate byte + demapped data bits)
% Output 3: Scatter plot I values (COMBINED I/Q, sent every scatterInterval packets)
% Output 4: Scatter plot Q values (NOT USED - kept for compatibility)
%
% Scatter output format (sent every scatterInterval packets):
%   First 4 bytes: uint32 number of symbols (little-endian)
%   Remaining bytes: Interleaved I,Q pairs as int16 (I[0],Q[0],I[1],Q[1],...)
%
% OFDM Symbol Structure (64 subcarriers, IEEE 802.11a):
%   256 bytes per OFDM symbol (64 subcarriers × 4 bytes)
%   Per subcarrier: I[2 bytes] + Q[2 bytes] (int16, little-endian, scaled by 32767)
%   
% Subcarrier allocation (IEEE 802.11a):
%   - 48 data subcarriers (used for scatter plot)
%   - 4 pilot subcarriers (NOT included in scatter plot)
%   - 12 null subcarriers (NOT included in scatter plot)
%
% Operation:
% 1. Read first OFDM symbol (SIGNAL) - extract 48 BPSK data symbols
% 2. Demap to 48 bits, pack to 6 bytes, send to deinterleaver SIGNAL input
% 3. Buffer SIGNAL data symbols (48 symbols, NO pilots/nulls)
% 4. Wait for rate_length feedback from ppdu_decapsulate
% 5. Calculate N_SYM OFDM symbols needed (MUST match encoder calculation)
% 6. Read N_SYM OFDM symbols and extract data symbols
% 7. Buffer all DATA symbols (48 per OFDM symbol, NO pilots/nulls)
% 8. Send rate + demapped bits to deinterleaver DATA input
% 9. Send ALL symbols (SIGNAL + DATA) to scatter plot in one message per pipe
%
% @BlockConfig
% name: QamDemapperOfdm
% inputs: 2
% outputs: 3
% inputSize: [256, 3]
% outputSize: [6, 3025, 50000]
% LTR: false
% startWithAll: true
% scatterInterval: 1
% description: QAM demapper - sends scatter data every packet, DATA symbols only, combined I/Q
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
        
        % Subcarrier mapping: data symbol index -> OFDM subcarrier index
        % IEEE 802.11a: 48 data subcarriers (excludes pilots at -21, -7, 7, 21 and nulls)
        data_subcarrier_map = [-26:-22, -20:-8, -6:-1, 1:6, 8:20, 22:26];
        
        send_protocol_message('BLOCK_READY', config.blockId, config.name, '');
        
        frameCount = 0;
        totalBytes = 0;
        startTime = tic;
        lastTime = 0;
        lastBytes = 0;
        
        % Scatter plot accumulation
        scatterInterval = 1;  % Send scatter data every N packets
        if isfield(config, 'scatterInterval')
            scatterInterval = config.scatterInterval;
        end
        scatterCounter = 0;
        scatterI_accum = [];  % Accumulate I values
        scatterQ_accum = [];  % Accumulate Q values
        scatterSampleRate = 10;  % Only send every Nth symbol to reduce UI load
        
        fprintf('Scatter plot: Sending every %d packets (DATA symbols only, no SIGNAL)\n', scatterInterval);
        fprintf('Scatter sampling: Every %d symbols (reduces UI load)\n\n', scatterSampleRate);
        
        while true
            frameCount = frameCount + 1;
            
            % ===== STEP 1: Read SIGNAL OFDM symbol (always BPSK) =====
            ofdmSymbolInput = pipeline_mex('read', pipeInOfdmSymbols, config.inputSize(1));
            
            % Extract 64 subcarriers
            subcarriers = extract_subcarriers(ofdmSymbolInput);
            
            % Extract 48 data symbols from data subcarrier positions (NO PILOTS)
            dataSymbols = zeros(1, 48);
            for i = 1:48
                sc_idx = data_subcarrier_map(i);
                matlab_idx = sc_idx + 33;
                dataSymbols(i) = subcarriers(matlab_idx);
            end
            
            % Demap BPSK symbols to bits
            signalBits = zeros(1, 48);
            for i = 1:48
                if real(dataSymbols(i)) < 0
                    signalBits(i) = 0;
                else
                    signalBits(i) = 1;
                end
            end
            
            % Pack SIGNAL bits to bytes
            signalBytes = zeros(6, 1, 'uint8');
            for i = 1:6
                bitStart = (i-1) * 8 + 1;
                signalBytes(i) = bi2de(signalBits(bitStart:bitStart+7), 'right-msb');
            end
            
            % Send SIGNAL to deinterleaver
            signalOutput = int8(int16(signalBytes) - 128);
            pipeline_mex('write', pipeOutSignal, signalOutput);
            
            % NOTE: We do NOT buffer SIGNAL symbols for scatter plot
            % Only DATA symbols will be sent to scatter plot
            
            % ===== STEP 2: Wait for rate_length feedback =====
            feedbackInput = pipeline_mex('read', pipeInFeedback, config.inputSize(2));
            rateValue = uint8(int32(feedbackInput(1)) + 128);
            lengthLow = uint8(int32(feedbackInput(2)) + 128);
            lengthHigh = uint8(int32(feedbackInput(3)) + 128);
            psduLength = uint16(lengthLow) + bitshift(uint16(lengthHigh), 8);
            
            % ===== STEP 3: Calculate number of DATA OFDM symbols =====
            % This MUST match the encoder calculation EXACTLY
            params = rateParams(rateValue);
            N_DBPS = params.NDBPS;
            NBPSC = params.NBPSC;
            NCBPS = params.NCBPS;
            modType = params.mod;
            
            % Match ppdu_encapsulate calculation:
            % L_FRAMING = psduLength * 8 (PSDU in bits, includes 1500 payload + 4 CRC)
            % L_CRC = L_FRAMING + 32 (already has CRC, but this is the total)
            % Actually, psduLength IS the PSDU (1500 + 4 CRC = 1504 bytes)
            % So L_FRAMING should be 1500*8 (just payload)
            % But the LENGTH field in SIGNAL is the PSDU length (1504)
            
            % Correct calculation to match encoder:
            L_FRAMING = 1500 * 8;  % Payload only
            L_CRC = L_FRAMING + 32;  % Add CRC
            L_PPDU = 16 + L_CRC + 6;  % SERVICE + (payload+CRC) + TAIL
            N_DATA = L_PPDU;
            
            % Number of OFDM symbols
            N_SYM = ceil(N_DATA / N_DBPS);
            
            % Total bits in all DATA OFDM symbols
            totalDataBits = N_SYM * NCBPS;
            
            % DEBUG: Print detailed calculation
            if frameCount <= 3
                fprintf('\n========================================\n');
                fprintf('DEMAPPER Frame %d - N_SYM Calculation\n', frameCount);
                fprintf('========================================\n');
                fprintf('PSDU Length from SIGNAL: %d bytes\n', psduLength);
                fprintf('Rate value: %d\n', rateValue);
                fprintf('Modulation: %s\n', modType);
                fprintf('N_DBPS (data bits/symbol): %d\n', N_DBPS);
                fprintf('NCBPS (coded bits/symbol): %d\n', NCBPS);
                fprintf('NBPSC (bits/subcarrier): %d\n', NBPSC);
                fprintf('\nBit count calculation:\n');
                fprintf('  L_FRAMING (payload): %d bits\n', L_FRAMING);
                fprintf('  L_CRC (payload+CRC): %d bits\n', L_CRC);
                fprintf('  L_PPDU (SVC+DATA+TAIL): %d bits\n', L_PPDU);
                fprintf('  N_DATA: %d bits\n', N_DATA);
                fprintf('\nOFDM symbols:\n');
                fprintf('  N_SYM = ceil(%d / %d) = %d\n', N_DATA, N_DBPS, N_SYM);
                fprintf('  Total coded bits to read: %d x %d = %d bits\n', N_SYM, NCBPS, totalDataBits);
                fprintf('\nWill read %d OFDM symbols from mapper\n', N_SYM);
                fprintf('========================================\n\n');
            end
            
            % ===== STEP 4: Read and demap DATA OFDM symbols =====
            dataBits = zeros(1, totalDataBits);
            bitIdx = 1;
            
            % Pre-allocate scatter buffer for all DATA symbols (48 per OFDM symbol)
            totalDataSymbols = N_SYM * 48;
            scatterI_data = zeros(1, totalDataSymbols, 'int16');
            scatterQ_data = zeros(1, totalDataSymbols, 'int16');
            scatterIdx = 1;
            
            for ofdmIdx = 1:N_SYM
                % Read OFDM symbol
                ofdmSymbolInput = pipeline_mex('read', pipeInOfdmSymbols, config.inputSize(1));
                
                % Extract subcarriers
                subcarriers = extract_subcarriers(ofdmSymbolInput);
                
                % Extract 48 data symbols (NO PILOTS)
                dataSymbols = zeros(1, 48);
                for i = 1:48
                    sc_idx = data_subcarrier_map(i);
                    matlab_idx = sc_idx + 33;
                    dataSymbols(i) = subcarriers(matlab_idx);
                end
                
                % Demap symbols to bits and buffer for scatter plot
                for i = 1:48
                    symbol = dataSymbols(i);
                    
                    % Buffer I and Q for scatter plot (DATA ONLY, as int16 scaled by 32767)
                    % Sample every Nth symbol to reduce UI load
                    if mod(scatterIdx - 1, scatterSampleRate) == 0
                        scatterI_data(ceil(scatterIdx / scatterSampleRate)) = int16(round(real(symbol) * 32767));
                        scatterQ_data(ceil(scatterIdx / scatterSampleRate)) = int16(round(imag(symbol) * 32767));
                    end
                    scatterIdx = scatterIdx + 1;
                    
                    % Demap to bits
                    switch modType
                        case 'BPSK'
                            symbolBits = demap_bpsk(symbol);
                        case 'QPSK'
                            symbolBits = demap_qpsk(symbol);
                        case '16QAM'
                            symbolBits = demap_16qam(symbol);
                        case '64QAM'
                            symbolBits = demap_64qam(symbol);
                    end
                    
                    dataBits(bitIdx:bitIdx+NBPSC-1) = symbolBits;
                    bitIdx = bitIdx + NBPSC;
                end
                
                % Progress indicator for first frame
                if frameCount == 1 && mod(ofdmIdx, 10) == 0
                    fprintf('  Read OFDM symbol %d/%d\n', ofdmIdx, N_SYM);
                end
            end
            
            if frameCount <= 3
                fprintf('  Finished reading all %d DATA OFDM symbols\n\n', N_SYM);
            end
            
            % Trim scatter arrays to actual sampled size
            actualSamples = ceil(totalDataSymbols / scatterSampleRate);
            scatterI_data = scatterI_data(1:actualSamples);
            scatterQ_data = scatterQ_data(1:actualSamples);
            
            % Pack bits to bytes
            numDataBytes = ceil(totalDataBits / 8);
            if mod(totalDataBits, 8) ~= 0
                padBits = 8 - mod(totalDataBits, 8);
                dataBits = [dataBits, zeros(1, padBits)];
            end
            
            dataBytes = zeros(numDataBytes, 1, 'uint8');
            for i = 1:numDataBytes
                bitStart = (i-1) * 8 + 1;
                dataBytes(i) = bi2de(dataBits(bitStart:bitStart+7), 'right-msb');
            end
            
            % ===== STEP 5: Send rate + DATA to deinterleaver =====
            dataOutput = zeros(config.outputSize(2), 1, 'int8');
            dataOutput(1) = int8(int32(rateValue) - 128);
            copyLen = min(numDataBytes, config.outputSize(2) - 1);
            dataOutput(2:copyLen+1) = int8(int16(dataBytes(1:copyLen)) - 128);
            
            pipeline_mex('write', pipeOutData, dataOutput);
            
            % ===== STEP 6: Accumulate DATA symbols for scatter plot =====
            % Accumulate only DATA symbols (no SIGNAL)
            scatterI_accum = [scatterI_accum, scatterI_data];
            scatterQ_accum = [scatterQ_accum, scatterQ_data];
            scatterCounter = scatterCounter + 1;
            
            % Send to scatter plot every scatterInterval packets
            if scatterCounter >= scatterInterval
                totalSymbols = uint32(length(scatterI_accum));
                
                % Create combined I/Q output (interleaved format)
                % Format: [numSymbols (4 bytes)] [I[0], Q[0], I[1], Q[1], ...]
                scatterOutput = zeros(config.outputSize(3), 1, 'int8');
                
                % Pack length (uint32, little-endian)
                lengthBytes = typecast(totalSymbols, 'uint8');
                scatterOutput(1:4) = int8(int16(lengthBytes) - 128);
                
                % Pack interleaved I/Q values (int16 pairs)
                for i = 1:totalSymbols
                    I_bytes = typecast(scatterI_accum(i), 'uint8');
                    Q_bytes = typecast(scatterQ_accum(i), 'uint8');
                    
                    baseIdx = 4 + (i-1)*4;  % Each symbol = 4 bytes (I + Q)
                    scatterOutput(baseIdx+1) = int8(int16(I_bytes(1)) - 128);  % I low byte
                    scatterOutput(baseIdx+2) = int8(int16(I_bytes(2)) - 128);  % I high byte
                    scatterOutput(baseIdx+3) = int8(int16(Q_bytes(1)) - 128);  % Q low byte
                    scatterOutput(baseIdx+4) = int8(int16(Q_bytes(2)) - 128);  % Q high byte
                end
                
                % Send combined I/Q to scatter plot
                pipeline_mex('write', pipeOutScatter, scatterOutput);
                
                if mod(frameCount, scatterInterval) == 0 || frameCount <= 3
                    fprintf('Frame %d: Sent %d DATA symbols to scatter (accumulated from %d packets)\n', ...
                            frameCount, totalSymbols, scatterCounter);
                end
                
                % Reset accumulation
                scatterI_accum = [];
                scatterQ_accum = [];
                scatterCounter = 0;
            end
            
            totalBytes = totalBytes + numDataBytes;
            
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
        fprintf('\nQAM DEMAPPER OFDM FATAL ERROR in frame %d\n', frameCount);
        fprintf('Message: %s\n', ME.message);
        fprintf('Stack trace:\n');
        for i = 1:length(ME.stack)
            fprintf('  %s (line %d)\n', ME.stack(i).name, ME.stack(i).line);
        end
        send_protocol_message('BLOCK_ERROR', config.blockId, config.name, ME.message);
        rethrow(ME);
    end
end

function subcarriers = extract_subcarriers(ofdmSymbolInput)
    % Extract 64 complex subcarriers from packed int8 format
    subcarriers = zeros(1, 64);
    
    for i = 1:64
        baseIdx = (i - 1) * 4;
        
        % Convert int8 to uint8
        bytes = uint8(int16(ofdmSymbolInput(baseIdx+1:baseIdx+4)) + 128);
        
        % Reconstruct int16 values using typecast
        I_val = double(typecast(uint8([bytes(1), bytes(2)]), 'int16'));
        Q_val = double(typecast(uint8([bytes(3), bytes(4)]), 'int16'));
        
        % Convert back to complex symbol (descale from int16 range)
        subcarriers(i) = (I_val + 1i * Q_val) / 32767.0;
    end
end

function bits = demap_bpsk(symbol)
    if real(symbol) < 0
        bits = 0;
    else
        bits = 1;
    end
end

function bits = demap_qpsk(symbol)
    bits = zeros(1, 2);
    if real(symbol) < 0
        bits(1) = 0;
    else
        bits(1) = 1;
    end
    if imag(symbol) < 0
        bits(2) = 0;
    else
        bits(2) = 1;
    end
end

function bits = demap_16qam(symbol)
    k = 1/sqrt(10);
    symbol = symbol / k;
    bits = zeros(1, 4);
    I = real(symbol);
    Q = imag(symbol);
    
    if I < -2
        bits(1:2) = [0, 0];
    elseif I < 0
        bits(1:2) = [0, 1];
    elseif I < 2
        bits(1:2) = [1, 1];
    else
        bits(1:2) = [1, 0];
    end
    
    if Q < -2
        bits(3:4) = [0, 0];
    elseif Q < 0
        bits(3:4) = [0, 1];
    elseif Q < 2
        bits(3:4) = [1, 1];
    else
        bits(3:4) = [1, 0];
    end
end

function bits = demap_64qam(symbol)
    k = 1/sqrt(42);
    symbol = symbol / k;
    bits = zeros(1, 6);
    I = real(symbol);
    Q = imag(symbol);
    
    if I < -6
        bits(1:3) = [0, 0, 0];
    elseif I < -4
        bits(1:3) = [0, 0, 1];
    elseif I < -2
        bits(1:3) = [0, 1, 1];
    elseif I < 0
        bits(1:3) = [0, 1, 0];
    elseif I < 2
        bits(1:3) = [1, 1, 0];
    elseif I < 4
        bits(1:3) = [1, 1, 1];
    elseif I < 6
        bits(1:3) = [1, 0, 1];
    else
        bits(1:3) = [1, 0, 0];
    end
    
    if Q < -6
        bits(4:6) = [0, 0, 0];
    elseif Q < -4
        bits(4:6) = [0, 0, 1];
    elseif Q < -2
        bits(4:6) = [0, 1, 1];
    elseif Q < 0
        bits(4:6) = [0, 1, 0];
    elseif Q < 2
        bits(4:6) = [1, 1, 0];
    elseif Q < 4
        bits(4:6) = [1, 1, 1];
    elseif Q < 6
        bits(4:6) = [1, 0, 1];
    else
        bits(4:6) = [1, 0, 0];
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
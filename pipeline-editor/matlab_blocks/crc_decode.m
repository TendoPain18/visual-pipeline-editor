function crc_decode(pipeIn, pipeOut)
% CRC_DECODE - ITU-T CRC-32 decoder using generic block framework

    % ========== USER CONFIGURATION ==========
    block_config = struct( ...
        'name',              'CrcDecode', ...
        'inputs',            1, ...
        'outputs',           1, ...
        'inputPacketSizes',  1504, ...
        'inputBatchSizes',   44740, ...
        'outputPacketSizes', 1501, ...
        'outputBatchSizes',  44740, ...
        'LTR',               false, ...
        'startWithAll',      true, ...
        'socketHost',        'localhost', ...
        'socketPort',        9001, ...
        'description',       'CRC-32 decoder with error detection - batch processing' ...
    );
    
    % ========== CUSTOM INITIALIZATION ==========
    init_fn = @(config) struct('crcTable', build_crc32_table(), 'errorCount', 0, 'packetCount', 0);
    
    % ========== PROCESSING FUNCTION ==========
    process_fn = @(inputBatch, actualCount, customData, config) ...
        process_crc_decode(inputBatch, actualCount, customData, config);
    
    % ========== RUN GENERIC BLOCK ==========
    run_generic_block(pipeIn, pipeOut, block_config, process_fn, init_fn);
end

function [outputBatch, actualOutputCount] = process_crc_decode(inputBatch, actualCount, customData, config)
    INPUT_PACKET_SIZE = config.inputPacketSizes(1);
    OUTPUT_PACKET_SIZE = config.outputPacketSizes(1);
    OUTPUT_BATCH_SIZE = config.outputBatchSizes(1);
    DATA_SIZE = INPUT_PACKET_SIZE - 4;  % Remove CRC bytes
    
    crcTable = customData.crcTable;
    
    % Pre-allocate output batch
    outputBatch = zeros(OUTPUT_BATCH_SIZE * OUTPUT_PACKET_SIZE, 1, 'int8');
    
    % Process each packet: check CRC and remove it
    for pktIdx = 1:actualCount
        % Extract input packet (data + CRC)
        inputOffset = (pktIdx - 1) * INPUT_PACKET_SIZE;
        inputPacket = inputBatch(inputOffset + 1 : inputOffset + INPUT_PACKET_SIZE);
        
        % Split data and CRC
        dataInt8 = inputPacket(1:DATA_SIZE);
        crcInt8 = inputPacket(DATA_SIZE + 1 : INPUT_PACKET_SIZE);
        
        % Calculate CRC on data
        dataAsUint8 = typecast(dataInt8, 'uint8');
        calculatedCrc = calculate_crc32(dataAsUint8, crcTable);
        
        % Extract received CRC
        crcBytes = typecast(crcInt8, 'uint8');
        receivedCrc = typecast(crcBytes, 'uint32');
        
        % Compare CRCs
        if receivedCrc == calculatedCrc
            errorFlag = int8(0);
        else
            errorFlag = int8(1);
            customData.errorCount = customData.errorCount + 1;
        end
        
        % Create output packet (data + error flag)
        outputOffset = (pktIdx - 1) * OUTPUT_PACKET_SIZE;
        outputBatch(outputOffset + 1 : outputOffset + DATA_SIZE) = dataInt8;
        outputBatch(outputOffset + OUTPUT_PACKET_SIZE) = errorFlag;
        
        customData.packetCount = customData.packetCount + 1;
    end
    
    actualOutputCount = actualCount;
    
    % Periodic error reporting
    if mod(customData.packetCount, 100000) == 0
        errorRate = 100.0 * customData.errorCount / customData.packetCount;
        fprintf('Packets: %d, Errors: %d (%.2f%%)\n', ...
                customData.packetCount, customData.errorCount, errorRate);
    end
end

function crcTable = build_crc32_table()
    poly = uint32(0xEDB88320);
    crcTable = zeros(256, 1, 'uint32');
    for i = 0:255
        crc = uint32(i);
        for j = 0:7
            if bitand(crc, uint32(1))
                crc = bitxor(bitshift(crc, -1), poly);
            else
                crc = bitshift(crc, -1);
            end
        end
        crcTable(i + 1) = crc;
    end
end

function crc = calculate_crc32(data, crcTable)
    crc = uint32(0xFFFFFFFF);
    for i = 1:length(data)
        tableIdx = bitxor(bitand(crc, uint32(0xFF)), uint32(data(i))) + 1;
        crc = bitxor(bitshift(crc, -8), crcTable(tableIdx));
    end
    crc = bitxor(crc, uint32(0xFFFFFFFF));
end
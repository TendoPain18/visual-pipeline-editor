function crc_decode(pipeIn, pipeOut)
% CRC_DECODE  ITU-T CRC-32 decoder with error detection — manual I/O version.

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

    run_generic_block({pipeIn}, {pipeOut}, block_config, @process_fn, @init_fn);
end

%% ─── Init ───────────────────────────────────────────────────────────────────
function state = init_fn(~)
    state = struct( ...
        'crcTable',   buildCrcTable(), ...
        'errorCount', 0, ...
        'pktCount',   0 ...
    );
    fprintf('[CrcDecode] CRC-32 table built\n');
end

%% ─── Process ────────────────────────────────────────────────────────────────
function process_fn(pipeIn, pipeOut, initState, ~)
    persistent state;
    if isempty(state)
        state = initState;
    end

    inp = pipeIn{1};
    out = pipeOut{1};

    IN_PKT   = inp.getPacketSize();   % 1504
    OUT_PKT  = out.getPacketSize();   % 1501
    DATA_SIZE = IN_PKT - 4;           % 1500

    % ── Read ──────────────────────────────────────────────────────────────────
    [inputBatch, actualCount] = inp.read();

    % ── Process ───────────────────────────────────────────────────────────────
    outputBatch = zeros(out.getBatchSize() * OUT_PKT, 1, 'int8');

    for i = 1:actualCount
        inOff  = (i-1) * IN_PKT;
        outOff = (i-1) * OUT_PKT;

        dataInt8 = inputBatch(inOff+1 : inOff+DATA_SIZE);
        crcInt8  = inputBatch(inOff+DATA_SIZE+1 : inOff+IN_PKT);

        % Calculate CRC on data
        dataU8 = uint8(int32(dataInt8) + 128);
        calcCrc = calcCrc32(dataU8, state.crcTable);

        % Extract received CRC
        crcU8 = uint8(int32(crcInt8) + 128);
        recvCrc = typecast(crcU8, 'uint32');

        % Compare
        if recvCrc == calcCrc
            errorFlag = int8(0);
        else
            errorFlag = int8(1);
            state.errorCount = state.errorCount + 1;
        end

        % Output: data + error flag
        outputBatch(outOff+1 : outOff+DATA_SIZE)   = dataInt8;
        outputBatch(outOff+OUT_PKT)                = errorFlag;

        state.pktCount = state.pktCount + 1;
    end

    % ── Write ─────────────────────────────────────────────────────────────────
    out.write(outputBatch, actualCount);

    % Periodic stats
    if state.pktCount > 0 && mod(state.pktCount, 100000) == 0
        fprintf('[CrcDecode] Packets: %d, Errors: %d (%.2f%%)\n', ...
            state.pktCount, state.errorCount, ...
            100*state.errorCount/state.pktCount);
    end
end

%% ─── CRC helpers ─────────────────────────────────────────────────────────────
function table = buildCrcTable()
    poly  = uint32(0xEDB88320);
    table = zeros(256,1,'uint32');
    for i = 0:255
        crc = uint32(i);
        for j = 0:7
            if bitand(crc,uint32(1))
                crc = bitxor(bitshift(crc,-1), poly);
            else
                crc = bitshift(crc,-1);
            end
        end
        table(i+1) = crc;
    end
end

function crc = calcCrc32(data, table)
    crc = uint32(0xFFFFFFFF);
    for i = 1:numel(data)
        idx = bitxor(bitand(crc,uint32(255)), uint32(data(i))) + 1;
        crc = bitxor(bitshift(crc,-8), table(idx));
    end
    crc = bitxor(crc, uint32(0xFFFFFFFF));
end
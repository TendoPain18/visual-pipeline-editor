classdef PipeIO
% PIPEIO  Low-level pipe read/write — full manual control over I/O timing.
%
% Used with run_manual_block.m.  Mirrors the C++ PipeIO class exactly.
%
% Construction:
%   io = PipeIO(pipeName, packetSize, batchSize)
%
% Read (blocks until data is ready):
%   [batchData, actualCount] = io.read()
%   batchData   -> int8 column vector, length = packetSize * batchSize
%   actualCount -> number of valid packets in this batch
%
% Write (blocks until consumer is ready):
%   io.write(batchData, actualCount)
%
% Getters:
%   io.getPacketSize()   io.getBatchSize()
%   io.getBufferSize()   io.getLengthBytes()
%   io.getName()

    properties (Access = private)
        pipeName_
        packetSize_
        batchSize_
        lengthBytes_
        bufferSize_
    end

    methods
        %% Constructor
        function obj = PipeIO(pipeName, packetSize, batchSize)
            obj.pipeName_    = pipeName;
            obj.packetSize_  = packetSize;
            obj.batchSize_   = batchSize;
            obj.lengthBytes_ = PipeIO.calcLengthBytes(batchSize);
            obj.bufferSize_  = obj.lengthBytes_ + packetSize * batchSize;
        end

        %% Read — blocks until data arrives
        function [batchData, actualCount] = read(obj)
            rawBuf      = pipeline_mex('read', obj.pipeName_, obj.bufferSize_);
            actualCount = PipeIO.decodeLength(rawBuf(1:obj.lengthBytes_), obj.lengthBytes_);
            batchData   = rawBuf(obj.lengthBytes_+1 : end);
        end

        %% Write — blocks until downstream is ready
        function write(obj, batchData, actualCount)
            buffer = zeros(obj.bufferSize_, 1, 'int8');

            % Encode actualCount into length header (little-endian)
            count32 = uint32(actualCount);
            for i = 1:obj.lengthBytes_
                byteVal   = bitand(bitshift(count32, -(i-1)*8), uint32(255));
                buffer(i) = int8(int32(byteVal) - 128);
            end

            % Copy data payload
            dataLen = min(numel(batchData), obj.bufferSize_ - obj.lengthBytes_);
            buffer(obj.lengthBytes_+1 : obj.lengthBytes_+dataLen) = batchData(1:dataLen);

            pipeline_mex('write', obj.pipeName_, buffer);
        end

        %% Getters
        function v = getPacketSize(obj);  v = obj.packetSize_;  end
        function v = getBatchSize(obj);   v = obj.batchSize_;   end
        function v = getBufferSize(obj);  v = obj.bufferSize_;  end
        function v = getLengthBytes(obj); v = obj.lengthBytes_; end
        function v = getName(obj);        v = obj.pipeName_;    end
    end

    methods (Static, Access = private)
        function lb = calcLengthBytes(maxCount)
            if     maxCount <= 255;       lb = 1;
            elseif maxCount <= 65535;     lb = 2;
            elseif maxCount <= 16777215;  lb = 3;
            else;                         lb = 4;
            end
        end

        function count = decodeLength(headerBytes, lb)
            count = 0;
            for i = 1:lb
                byteVal = double(uint8(int32(headerBytes(i)) + 128));
                count   = count + byteVal * (256^(i-1));
            end
        end
    end
end

function send_socket_message(socketObj, msgType, blockId, blockName, data)
% SEND_SOCKET_MESSAGE - Send JSON message via socket (SOCKET-ONLY VERSION)
%
% Usage:
%   send_socket_message(socket, 'BLOCK_INIT', 123, 'FileSource', '')
%   send_socket_message(socket, 'BLOCK_METRICS', 123, 'FileSource', metrics)
%
% Inputs:
%   socketObj - TCP client object from matlab_socket_client()
%   msgType   - Message type: BLOCK_INIT, BLOCK_READY, BLOCK_METRICS, etc.
%   blockId   - Block ID number
%   blockName - Block name string
%   data      - Data (struct for BLOCK_METRICS, string for errors)

    % Check if socket is valid
    if isempty(socketObj) || ~isvalid(socketObj)
        error('Socket is not connected. Cannot send message.');
    end
    
    timestamp = datestr(now, 'HH:MM:SS.FFF');
    
    % Build message structure
    msg = struct();
    msg.protocol = 'MATLAB_V1';
    msg.timestamp = timestamp;
    msg.blockId = blockId;
    msg.blockName = blockName;
    msg.type = msgType;
    
    % Add data based on type
    switch msgType
        case 'BLOCK_INIT'
            msg.data = struct('status', 'initializing');
            
        case 'BLOCK_READY'
            msg.data = struct('status', 'ready');
            
        case 'BLOCK_ERROR'
            if isstruct(data)
                msg.data = data;
            else
                msg.data = struct('error', char(data));
            end
            
        case 'BLOCK_METRICS'
            % Data should already be a struct with fields:
            % frames, gbps, totalGB (optional), totalFrames (optional)
            msg.data = data;
            
        case 'BLOCK_GRAPH'
            % Data should be struct with x, y fields
            msg.data = data;
            
        case 'BLOCK_STOPPING'
            msg.data = struct('status', 'stopping');
            
        case 'BLOCK_STOPPED'
            msg.data = struct('status', 'stopped');
            
        otherwise
            msg.data = struct('raw', char(data));
    end
    
    % Convert to JSON and send
    try
        jsonStr = jsonencode(msg);
        write(socketObj, uint8([jsonStr newline]));
    catch ME
        error('Failed to send %s message: %s', msgType, ME.message);
    end
end
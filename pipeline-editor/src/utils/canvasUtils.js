import { BLOCK_WIDTH, BLOCK_HEIGHT } from './blockUtils';

export const getPortPosition = (block, portType, portIndex) => {
  const ports = portType === 'input' ? block.portPositions.inputs : block.portPositions.outputs;
  const port = ports[portIndex];
  if (!port) return { x: block.x + BLOCK_WIDTH / 2, y: block.y + BLOCK_HEIGHT / 2 };
  return { x: block.x + port.x, y: block.y + port.y };
};

export const getPortAtPosition = (x, y, blocks) => {
  for (const block of blocks) {
    for (let i = 0; i < block.portPositions.inputs.length; i++) {
      const pos = getPortPosition(block, 'input', i);
      const dist = Math.sqrt(Math.pow(x - pos.x, 2) + Math.pow(y - pos.y, 2));
      if (dist < 12) {
        return { blockId: block.id, type: 'input', index: i };
      }
    }
    
    for (let i = 0; i < block.portPositions.outputs.length; i++) {
      const pos = getPortPosition(block, 'output', i);
      const dist = Math.sqrt(Math.pow(x - pos.x, 2) + Math.pow(y - pos.y, 2));
      if (dist < 12) {
        return { blockId: block.id, type: 'output', index: i };
      }
    }
  }
  return null;
};

export const getWaypointAtPosition = (x, y, connections) => {
  for (const conn of connections) {
    if (conn.waypoints) {
      for (let i = 0; i < conn.waypoints.length; i++) {
        const wp = conn.waypoints[i];
        const dist = Math.sqrt(Math.pow(x - wp.x, 2) + Math.pow(y - wp.y, 2));
        if (dist < 12) {
          return { connectionId: conn.id, waypointIndex: i };
        }
      }
    }
  }
  return null;
};

export const getConnectionAtPoint = (x, y, connections, blocks, snapToGrid) => {
  for (const conn of connections) {
    const fromBlock = blocks.find(b => b.id === conn.fromBlock);
    const toBlock = blocks.find(b => b.id === conn.toBlock);
    
    if (fromBlock && toBlock) {
      const startPos = getPortPosition(fromBlock, 'output', conn.fromPort);
      const endPos = getPortPosition(toBlock, 'input', conn.toPort);
      
      const points = [startPos];
      if (conn.waypoints) points.push(...conn.waypoints);
      points.push(endPos);
      
      for (let i = 0; i < points.length - 1; i++) {
        const p1 = points[i];
        const p2 = points[i + 1];
        
        const A = x - p1.x;
        const B = y - p1.y;
        const C = p2.x - p1.x;
        const D = p2.y - p1.y;
        
        const dot = A * C + B * D;
        const lenSq = C * C + D * D;
        const param = lenSq !== 0 ? dot / lenSq : -1;
        
        let xx, yy;
        
        if (param < 0) {
          xx = p1.x;
          yy = p1.y;
        } else if (param > 1) {
          xx = p2.x;
          yy = p2.y;
        } else {
          xx = p1.x + param * C;
          yy = p1.y + param * D;
        }
        
        const dist = Math.sqrt(Math.pow(x - xx, 2) + Math.pow(y - yy, 2));
        
        if (dist < 10) {
          return { connection: conn, insertAfter: i, position: { x: snapToGrid(x), y: snapToGrid(y) } };
        }
      }
    }
  }
  return null;
};

/**
 * STRICT BATCH MATCHING VALIDATION
 * 
 * Both packet size AND batch size must match exactly.
 * No automatic conversions allowed.
 * 
 * Connection format: packetSize × batchSize (matrix dimensions)
 */
export const validateConnection = (fromBlock, fromPort, toBlock, toPort, blocks, addLog) => {
  const sourceBlock = blocks.find(b => b.id === fromBlock);
  const targetBlock = blocks.find(b => b.id === toBlock);
  
  if (!sourceBlock || !targetBlock) return false;
  
  // Get batch parameters for the specific ports
  const getPortBatchParams = (block, portIndex, isOutput) => {
    if (isOutput) {
      const packetSizes = block.outputPacketSizes || [];
      const batchSizes = block.outputBatchSizes || [];
      return {
        packetSize: packetSizes[portIndex] || 0,
        batchSize: batchSizes[portIndex] || 1,
        lengthBytes: block.outputLengthBytes ? block.outputLengthBytes[portIndex] : 0,
        bufferSize: block.outputBufferSizes ? block.outputBufferSizes[portIndex] : 0
      };
    } else {
      const packetSizes = block.inputPacketSizes || [];
      const batchSizes = block.inputBatchSizes || [];
      return {
        packetSize: packetSizes[portIndex] || 0,
        batchSize: batchSizes[portIndex] || 1,
        lengthBytes: block.inputLengthBytes ? block.inputLengthBytes[portIndex] : 0,
        bufferSize: block.inputBufferSizes ? block.inputBufferSizes[portIndex] : 0
      };
    }
  };
  
  const source = getPortBatchParams(sourceBlock, fromPort, true);
  const target = getPortBatchParams(targetBlock, toPort, false);
  
  // STRICT VALIDATION: Both dimensions must match
  
  // Check 1: Packet size must match exactly
  if (source.packetSize !== target.packetSize) {
    addLog('error', 
      `❌ Packet size mismatch:\n` +
      `${sourceBlock.name} output port ${fromPort}: ${source.packetSize} bytes/packet\n` +
      `${targetBlock.name} input port ${toPort}: ${target.packetSize} bytes/packet\n` +
      `Matrix dimensions must match exactly.`
    );
    return false;
  }
  
  // Check 2: Batch size must match exactly
  if (source.batchSize !== target.batchSize) {
    addLog('error', 
      `❌ Batch size mismatch:\n` +
      `${sourceBlock.name} output port ${fromPort}: ${source.batchSize} packets/batch\n` +
      `${targetBlock.name} input port ${toPort}: ${target.batchSize} packets/batch\n` +
      `Matrix dimensions must match exactly.`
    );
    return false;
  }
  
  // Check 3: Buffer size should match (including length header)
  if (source.bufferSize !== target.bufferSize) {
    addLog('error', 
      `❌ Buffer size mismatch:\n` +
      `${sourceBlock.name} output port ${fromPort}: ${source.bufferSize} bytes total\n` +
      `${targetBlock.name} input port ${toPort}: ${target.bufferSize} bytes total\n` +
      `This shouldn't happen if packet and batch sizes match - check configuration.`
    );
    return false;
  }
  
  // All checks passed
  addLog('success', 
    `✅ Connection valid: ${source.packetSize}×${source.batchSize} ` +
    `(${(source.bufferSize / 1024).toFixed(2)} KB total)`
  );
  
  return true;
};
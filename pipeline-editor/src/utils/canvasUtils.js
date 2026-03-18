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

export const validateConnection = (fromBlock, fromPort, toBlock, toPort, blocks, addLog) => {
  const sourceBlock = blocks.find(b => b.id === fromBlock);
  const targetBlock = blocks.find(b => b.id === toBlock);
  
  if (!sourceBlock || !targetBlock) return false;
  
  // Get the size for the specific port
  const getPortSize = (block, portIndex, isOutput) => {
    const sizes = isOutput ? block.outputSizes : block.inputSizes;
    if (Array.isArray(sizes) && sizes.length > portIndex) {
      return sizes[portIndex];
    }
    // Fallback to single size
    return isOutput ? block.outputSize : block.inputSize;
  };
  
  const sourceSize = getPortSize(sourceBlock, fromPort, true);
  const targetSize = getPortSize(targetBlock, toPort, false);
  
  if (sourceSize !== targetSize) {
    addLog('error', `Size mismatch: ${sourceBlock.name} output port ${fromPort} (${(sourceSize / 1024 / 1024).toFixed(2)}MB) ` +
                    `!= ${targetBlock.name} input port ${toPort} (${(targetSize / 1024 / 1024).toFixed(2)}MB)`);
    return false;
  }
  
  return true;
};
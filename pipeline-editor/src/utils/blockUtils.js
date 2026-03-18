export const GRID_SIZE = 20;
export const BLOCK_WIDTH = 140;
export const BLOCK_HEIGHT = 80;

export const parseMatlabBlock = (fileContent, fileName) => {
  const blockName = fileName.replace('.m', '');
  
  const configMatch = fileContent.match(/@BlockConfig([\s\S]*?)@EndBlockConfig/);
  if (!configMatch) {
    throw new Error('No @BlockConfig section found');
  }
  
  const configText = configMatch[1];
  const config = {};
  const lines = configText.split('\n');
  
  lines.forEach((line) => {
    line = line.trim();
    if (line.startsWith('%')) {
      line = line.substring(1).trim();
    }
    
    if (!line) return;
    
    const colonIndex = line.indexOf(':');
    if (colonIndex === -1) return;
    
    let key = line.substring(0, colonIndex).trim();
    let value = line.substring(colonIndex + 1).trim();
    
    const commentIndex = value.indexOf('%');
    if (commentIndex !== -1) {
      value = value.substring(0, commentIndex).trim();
    }
    
    if (value === 'true' || value === 'false') {
      config[key] = value === 'true';
    } else if (value.includes('*')) {
      try {
        config[key] = eval(value.replace(/\*/g, '*'));
      } catch {
        config[key] = value;
      }
    } else if (value.startsWith('[') && value.endsWith(']')) {
      try {
        config[key] = eval(value);
      } catch {
        config[key] = value;
      }
    } else {
      const numValue = Number(value);
      if (!isNaN(numValue) && value !== '') {
        config[key] = numValue;
      } else {
        config[key] = value;
      }
    }
  });
  
  const numInputs = typeof config.inputs === 'number' ? config.inputs : 1;
  const numOutputs = typeof config.outputs === 'number' ? config.outputs : 1;
  
  let inputSize = config.inputSize || 0;
  let outputSize = config.outputSize || 0;
  
  const inputSizes = Array.isArray(inputSize) ? inputSize : [inputSize];
  const outputSizes = Array.isArray(outputSize) ? outputSize : [outputSize];
  
  const totalInputSize = inputSizes.reduce((a, b) => a + b, 0);
  const totalOutputSize = outputSizes.reduce((a, b) => a + b, 0);
  
  let sizeRelation;
  
  if (totalInputSize === 0) {
    sizeRelation = { type: 'source', description: 'Data source' };
  } else if (totalOutputSize === 0) {
    sizeRelation = { type: 'sink', description: 'Data sink' };
  } else if (totalOutputSize === totalInputSize) {
    sizeRelation = { type: 'same', description: 'Same size' };
  } else if (totalOutputSize > totalInputSize) {
    const ratio = totalOutputSize / totalInputSize;
    sizeRelation = { 
      type: 'multiply', 
      factor: ratio, 
      description: `×${ratio} (${config.description || 'increases size'})` 
    };
  } else {
    const ratio = totalInputSize / totalOutputSize;
    sizeRelation = { 
      type: 'divide', 
      factor: ratio, 
      description: `÷${ratio} (${config.description || 'decreases size'})` 
    };
  }
  
  const ltr = config.LTR !== undefined ? config.LTR : true;
  const startWithAll = config.startWithAll !== undefined ? config.startWithAll : false;
  const isGraph = config.graphType !== undefined;
  
  return {
    name: config.name || blockName,
    fileName: fileName,
    code: fileContent,
    inputs: numInputs,
    outputs: numOutputs,
    inputSize: inputSize,
    outputSize: outputSize,
    inputSizes: inputSizes,
    outputSizes: outputSizes,
    description: config.description || 'No description',
    config: config,
    color: isGraph ? '#9333ea' : generateColor(config.name || blockName),
    portPositions: calculatePortPositions(numInputs, numOutputs, ltr, BLOCK_WIDTH, BLOCK_HEIGHT, GRID_SIZE),
    sizeRelation: sizeRelation,
    ltr: ltr,
    startWithAll: startWithAll,
    isGraph: isGraph,
    graphType: config.graphType || null
  };
};

export const generateColor = (name) => {
  const colors = ['#3b82f6', '#8b5cf6', '#ec4899', '#f59e0b', '#6366f1'];
  let hash = 0;
  for (let i = 0; i < name.length; i++) {
    hash = name.charCodeAt(i) + ((hash << 5) - hash);
  }
  return colors[Math.abs(hash) % colors.length];
};

export const calculatePortPositions = (numInputs, numOutputs, ltr, BLOCK_WIDTH, BLOCK_HEIGHT, GRID_SIZE) => {
  const inputPorts = [];
  const outputPorts = [];
  
  const calculatePorts = (numPorts) => {
    const ports = [];
    const centerY = BLOCK_HEIGHT / 2;
    
    if (numPorts === 1) {
      ports.push(centerY);
    } else if (numPorts === 2) {
      ports.push(centerY - GRID_SIZE);
      ports.push(centerY + GRID_SIZE);
    } else if (numPorts === 3) {
      ports.push(centerY - GRID_SIZE);
      ports.push(centerY);
      ports.push(centerY + GRID_SIZE);
    } else {
      const spacing = GRID_SIZE;
      const totalHeight = (numPorts - 1) * spacing;
      const startY = centerY - totalHeight / 2;
      
      for (let i = 0; i < numPorts; i++) {
        ports.push(startY + i * spacing);
      }
    }
    
    return ports;
  };
  
  const inputYPositions = calculatePorts(numInputs);
  const outputYPositions = calculatePorts(numOutputs);
  
  const inputX = ltr ? 0 : BLOCK_WIDTH;
  const outputX = ltr ? BLOCK_WIDTH : 0;
  
  for (let i = 0; i < numInputs; i++) {
    inputPorts.push({ x: inputX, y: inputYPositions[i], type: 'input', index: i });
  }
  
  for (let i = 0; i < numOutputs; i++) {
    outputPorts.push({ x: outputX, y: outputYPositions[i], type: 'output', index: i });
  }
  
  return { inputs: inputPorts, outputs: outputPorts };
};

export const topologicalSort = (blocks, connections) => {
  // Find ALL source blocks (blocks with no inputs)
  const sources = blocks.filter(b => b.inputs === 0);
  
  if (sources.length === 0) {
    throw new Error('No source blocks found');
  }
  
  console.log(`[TopologicalSort] Found ${sources.length} source block(s):`, sources.map(s => s.name));
  
  // DETECT FEEDBACK CONNECTIONS using cycle detection
  const feedbackConnections = new Set();
  
  // Build forward adjacency map
  const forwardMap = new Map();
  blocks.forEach(block => {
    forwardMap.set(block.id, []);
  });
  
  connections.forEach(conn => {
    if (forwardMap.has(conn.fromBlock)) {
      forwardMap.get(conn.fromBlock).push({ blockId: conn.toBlock, connId: conn.id });
    }
  });
  
  // DFS to detect cycles - mark edges that create cycles as feedback
  const visited = new Set();
  const recStack = new Set();
  
  const detectCycle = (nodeId, path = []) => {
    visited.add(nodeId);
    recStack.add(nodeId);
    
    const neighbors = forwardMap.get(nodeId) || [];
    for (const { blockId: neighborId, connId } of neighbors) {
      if (!visited.has(neighborId)) {
        if (detectCycle(neighborId, [...path, connId])) {
          return true;
        }
      } else if (recStack.has(neighborId)) {
        // Found a cycle! Mark the connection that closes the loop as feedback
        feedbackConnections.add(connId);
        const fromBlock = blocks.find(b => b.id === nodeId);
        const toBlock = blocks.find(b => b.id === neighborId);
        console.log(`[TopologicalSort] Detected FEEDBACK connection: ${fromBlock?.name} → ${toBlock?.name} (connId: ${connId})`);
        return true;
      }
    }
    
    recStack.delete(nodeId);
    return false;
  };
  
  // Run cycle detection from all source blocks
  sources.forEach(source => {
    if (!visited.has(source.id)) {
      detectCycle(source.id);
    }
  });
  
  console.log(`[TopologicalSort] Identified ${feedbackConnections.size} feedback connection(s)`);
  
  // Build adjacency map WITHOUT feedback connections
  const adjacencyMap = new Map();
  blocks.forEach(block => {
    adjacencyMap.set(block.id, []);
  });
  
  connections.forEach(conn => {
    if (!feedbackConnections.has(conn.id)) {
      if (adjacencyMap.has(conn.fromBlock)) {
        adjacencyMap.get(conn.fromBlock).push(conn.toBlock);
      }
    }
  });
  
  console.log('[TopologicalSort] Adjacency map (without feedback):', Object.fromEntries(adjacencyMap));
  
  // BFS topological sort
  const sorted = [...sources];
  const visitedNodes = new Set(sources.map(s => s.id));
  
  let currentLevel = [...sources];
  let levelNum = 0;
  
  while (currentLevel.length > 0) {
    levelNum++;
    console.log(`[TopologicalSort] Processing level ${levelNum}:`, currentLevel.map(b => b.name));
    const nextLevel = [];
    
    for (const currentBlock of currentLevel) {
      const connectedBlockIds = adjacencyMap.get(currentBlock.id) || [];
      
      for (const targetId of connectedBlockIds) {
        if (!visitedNodes.has(targetId)) {
          const targetBlock = blocks.find(b => b.id === targetId);
          
          // Get only non-feedback input connections
          const inputConnections = connections.filter(c => {
            const isFeedback = feedbackConnections.has(c.id);
            return c.toBlock === targetId && !isFeedback;
          });
          
          // Check if all forward inputs are ready
          const allInputsReady = inputConnections.every(conn => 
            visitedNodes.has(conn.fromBlock)
          );
          
          if (allInputsReady) {
            console.log(`[TopologicalSort] ✓ Adding ${targetBlock.name}`);
            visitedNodes.add(targetId);
            sorted.push(targetBlock);
            nextLevel.push(targetBlock);
          } else {
            const waitingFor = inputConnections
              .filter(conn => !visitedNodes.has(conn.fromBlock))
              .map(conn => blocks.find(b => b.id === conn.fromBlock)?.name || conn.fromBlock);
            console.log(`[TopologicalSort] ⏸ Skipping ${targetBlock.name} (waiting for: ${waitingFor.join(', ')})`);
          }
        }
      }
    }
    
    currentLevel = nextLevel;
  }
  
  // Check if all blocks were included
  if (sorted.length !== blocks.length) {
    const missing = blocks.filter(b => !visitedNodes.has(b.id));
    console.warn('[TopologicalSort] ⚠ WARNING: Some blocks not in topological order:', missing.map(b => b.name));
    
    // Add missing blocks to the end
    missing.forEach(block => {
      sorted.push(block);
      console.warn(`[TopologicalSort] Added ${block.name} to end of sequence`);
    });
  } else {
    console.log('[TopologicalSort] ✅ All blocks successfully sorted');
  }
  
  console.log('[TopologicalSort] Final startup order:', sorted.map(b => b.name));
  
  return sorted;
};
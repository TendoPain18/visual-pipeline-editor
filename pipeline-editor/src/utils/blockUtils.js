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
      // Parse array
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
  
  // Handle inputSize and outputSize (can be number or array)
  let inputSize = config.inputSize || 0;
  let outputSize = config.outputSize || 0;
  
  // Convert to array if needed
  const inputSizes = Array.isArray(inputSize) ? inputSize : [inputSize];
  const outputSizes = Array.isArray(outputSize) ? outputSize : [outputSize];
  
  // Calculate total sizes for validation
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
  const colors = ['#3b82f6', '#8b5cf6', '#ec4899', '#f59e0b', '#10b981', '#06b6d4', '#6366f1'];
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
  
  // Start with all source blocks
  const sorted = [...sources];
  const visited = new Set(sources.map(s => s.id));
  
  // Build adjacency map for faster lookups
  const adjacencyMap = new Map();
  blocks.forEach(block => {
    adjacencyMap.set(block.id, []);
  });
  
  connections.forEach(conn => {
    if (adjacencyMap.has(conn.fromBlock)) {
      adjacencyMap.get(conn.fromBlock).push(conn.toBlock);
    }
  });
  
  console.log('[TopologicalSort] Adjacency map:', Object.fromEntries(adjacencyMap));
  
  // Process blocks level by level
  let currentLevel = [...sources];
  let levelNum = 0;
  
  while (currentLevel.length > 0) {
    levelNum++;
    console.log(`[TopologicalSort] Processing level ${levelNum}:`, currentLevel.map(b => b.name));
    const nextLevel = [];
    
    for (const currentBlock of currentLevel) {
      const connectedBlockIds = adjacencyMap.get(currentBlock.id) || [];
      
      for (const targetId of connectedBlockIds) {
        if (!visited.has(targetId)) {
          // Check if all inputs to this block have been visited
          const targetBlock = blocks.find(b => b.id === targetId);
          const inputConnections = connections.filter(c => c.toBlock === targetId);
          
          const allInputsReady = inputConnections.every(conn => 
            visited.has(conn.fromBlock)
          );
          
          if (allInputsReady) {
            console.log(`[TopologicalSort] Adding ${targetBlock.name} (all inputs ready)`);
            visited.add(targetId);
            sorted.push(targetBlock);
            nextLevel.push(targetBlock);
          } else {
            console.log(`[TopologicalSort] Skipping ${targetBlock.name} (waiting for inputs)`);
          }
        }
      }
    }
    
    currentLevel = nextLevel;
  }
  
  // Check if all blocks were included
  if (sorted.length !== blocks.length) {
    const missing = blocks.filter(b => !visited.has(b.id));
    console.warn('[TopologicalSort] WARNING: Some blocks were not included in topological sort:', missing.map(b => b.name));
    console.warn('[TopologicalSort] These blocks may be disconnected or have circular dependencies');
  } else {
    console.log('[TopologicalSort] All blocks successfully sorted');
  }
  
  console.log('[TopologicalSort] Final order:', sorted.map(b => b.name));
  
  return sorted;
};
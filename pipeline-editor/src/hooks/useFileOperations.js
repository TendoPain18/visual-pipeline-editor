import { parseMatlabBlock, calculatePortPositions, BLOCK_WIDTH, BLOCK_HEIGHT, GRID_SIZE } from '../utils/blockUtils';

export const useFileOperations = ({
  blocks,
  connections,
  projectDir,
  addLog,
  updateBothWithHistory,
  updateBlocksWithHistory,
  snapToGrid
}) => {
  const handleFileUpload = async () => {
    try {
      const filepaths = await window.electronAPI.selectFile([
        { name: 'MATLAB Files', extensions: ['m'] }
      ]);
      
      if (!filepaths) return;
      
      const files = Array.isArray(filepaths) ? filepaths : [filepaths];
      
      if (files.length === 0) return;
      
      let importedCount = 0;
      const newBlocks = [];
      
      for (const filepath of files) {
        try {
          addLog('info', `Reading file: ${filepath}`);
          const content = await window.electronAPI.readFile(filepath);
          const fileName = filepath.split(/[\\/]/).pop();
          
          addLog('info', `Parsing block: ${fileName}`);
          const blockData = parseMatlabBlock(content, fileName);
          
          if (blocks.some(b => b.name === blockData.name) || newBlocks.some(b => b.name === blockData.name)) {
            addLog('warning', `Skipping duplicate block: ${blockData.name}`);
            continue;
          }

          const newBlock = {
            id: Date.now() + importedCount * 100,
            ...blockData,
            x: snapToGrid(100),
            y: snapToGrid(100 + ((blocks.length + importedCount) * 30)),
            zIndex: blocks.length + importedCount
          };

          newBlocks.push(newBlock);
          importedCount++;
        } catch (error) {
          addLog('error', `Failed to import ${filepath.split(/[\\/]/).pop()}: ${error.message}`);
        }
      }
      
      if (newBlocks.length > 0) {
        updateBlocksWithHistory([...blocks, ...newBlocks]);
        addLog('success', `Imported ${newBlocks.length} block(s)`);
        return newBlocks;
      }
    } catch (error) {
      addLog('error', `Import failed: ${error.message}`);
    }
    return [];
  };

  const handleSaveDiagram = async () => {
    try {
      const diagram = {
        version: '1.0',
        blocks: blocks,
        connections: connections,
        timestamp: new Date().toISOString()
      };
      
      const filepath = await window.electronAPI.saveFileDialog('pipeline_diagram.psd', [
        { name: 'Pipeline Diagram', extensions: ['psd'] }
      ]);
      
      if (!filepath) return;
      
      await window.electronAPI.writeFile(filepath, JSON.stringify(diagram, null, 2));
      addLog('success', 'Diagram saved successfully');
    } catch (error) {
      addLog('error', 'Save failed: ' + error.message);
    }
  };

  const handleLoadDiagram = async () => {
    try {
      const filepath = await window.electronAPI.selectFile([
        { name: 'Pipeline Diagram', extensions: ['psd'] }
      ]);
      
      if (!filepath) return;
      
      const content = await window.electronAPI.readFile(filepath);
      const diagram = JSON.parse(content);
      
      updateBothWithHistory(diagram.blocks, diagram.connections);
      addLog('success', `Loaded diagram with ${diagram.blocks.length} blocks and ${diagram.connections.length} connections`);
    } catch (error) {
      addLog('error', 'Load failed: ' + error.message);
    }
  };

  return {
    handleFileUpload,
    handleSaveDiagram,
    handleLoadDiagram
  };
};

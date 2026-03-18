import { parseMatlabBlock } from '../utils/blockUtils';
import { parseCppBlock, isCppBlock } from '../utils/cppBlockUtils';

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
        { name: 'Block Files', extensions: ['m', 'cpp'] },
        { name: 'MATLAB Files', extensions: ['m'] },
        { name: 'C++ Files', extensions: ['cpp'] }
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
          const fileExt = fileName.split('.').pop().toLowerCase();
          
          let blockData;
          
          // Detect language and parse accordingly
          if (fileExt === 'm') {
            addLog('info', `Parsing MATLAB block: ${fileName}`);
            blockData = parseMatlabBlock(content, fileName);
            blockData.language = 'matlab';
          } else if (fileExt === 'cpp') {
            if (isCppBlock(content)) {
              addLog('info', `Parsing C++ block: ${fileName}`);
              blockData = parseCppBlock(content, fileName);
              blockData.language = 'cpp';
            } else {
              throw new Error('Not a valid C++ block - missing required headers');
            }
          } else {
            addLog('warning', `Unsupported file type: ${fileExt}`);
            continue;
          }
          
          if (blocks.some(b => b.name === blockData.name) || newBlocks.some(b => b.name === blockData.name)) {
            addLog('warning', `Skipping duplicate block: ${blockData.name}`);
            continue;
          }

          // Use safe block ID from main process
          const blockId = await window.electronAPI.getNextBlockId();
          
          const newBlock = {
            id: blockId,
            ...blockData,
            x: snapToGrid(100),
            y: snapToGrid(100 + ((blocks.length + importedCount) * 30)),
            zIndex: blocks.length + importedCount
          };

          newBlocks.push(newBlock);
          importedCount++;
          addLog('success', `Imported ${blockData.language.toUpperCase()} block: ${blockData.name} (ID: ${blockId})`);
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
        version: '2.0',
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
      const filepaths = await window.electronAPI.selectFile([
        { name: 'Pipeline Diagram', extensions: ['psd'] }
      ]);
      
      if (!filepaths || filepaths.length === 0) return;
      
      const filepath = Array.isArray(filepaths) ? filepaths[0] : filepaths;
      
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
import { useState, useEffect } from 'react';

export const useHistory = (initialBlocks = [], initialConnections = []) => {
  const [history, setHistory] = useState([]);
  const [historyIndex, setHistoryIndex] = useState(-1);
  const [blocks, setBlocks] = useState(initialBlocks);
  const [connections, setConnections] = useState(initialConnections);

  useEffect(() => {
    if (history.length === 0) {
      setHistory([{ blocks: [], connections: [] }]);
      setHistoryIndex(0);
    }
  }, []);

  const saveToHistory = (newBlocks, newConnections) => {
    const newState = {
      blocks: JSON.parse(JSON.stringify(newBlocks)),
      connections: JSON.parse(JSON.stringify(newConnections))
    };
    
    const newHistory = history.slice(0, historyIndex + 1);
    newHistory.push(newState);
    
    if (newHistory.length > 50) {
      newHistory.shift();
    } else {
      setHistoryIndex(historyIndex + 1);
    }
    
    setHistory(newHistory);
  };

  const updateBlocksWithHistory = (newBlocks) => {
    setBlocks(newBlocks);
    saveToHistory(newBlocks, connections);
  };

  const updateConnectionsWithHistory = (newConnections) => {
    setConnections(newConnections);
    saveToHistory(blocks, newConnections);
  };

  const updateBothWithHistory = (newBlocks, newConnections) => {
    setBlocks(newBlocks);
    setConnections(newConnections);
    saveToHistory(newBlocks, newConnections);
  };

  const undo = () => {
    if (historyIndex > 0) {
      const newIndex = historyIndex - 1;
      setHistoryIndex(newIndex);
      const state = history[newIndex];
      setBlocks(state.blocks);
      setConnections(state.connections);
      return true;
    }
    return false;
  };

  const redo = () => {
    if (historyIndex < history.length - 1) {
      const newIndex = historyIndex + 1;
      setHistoryIndex(newIndex);
      const state = history[newIndex];
      setBlocks(state.blocks);
      setConnections(state.connections);
      return true;
    }
    return false;
  };

  return {
    blocks,
    setBlocks,
    connections,
    setConnections,
    updateBlocksWithHistory,
    updateConnectionsWithHistory,
    updateBothWithHistory,
    undo,
    redo,
    canUndo: historyIndex > 0,
    canRedo: historyIndex < history.length - 1,
    saveToHistory
  };
};

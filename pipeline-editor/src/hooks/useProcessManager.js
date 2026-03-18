import { useState, useEffect, useRef } from 'react';
import { parseProtocolMessage, parseMetricsData, parseGraphData } from '../utils/protocolParser';

export const useProcessManager = (addLog) => {
  const [blockProcesses, setBlockProcesses] = useState({});
  const [blockMetrics, setBlockMetrics] = useState({});
  const [graphData, setGraphData] = useState({});
  const [blockStatus, setBlockStatus] = useState({});
  const processOutputListenerRef = useRef(null);

  useEffect(() => {
    if (processOutputListenerRef.current) {
      processOutputListenerRef.current();
    }
    
    processOutputListenerRef.current = window.electronAPI.onProcessOutput((data) => {
      if (data.type === 'started') {
        addLog('info', `${data.name || 'Process'}: Process started (PID: ${data.pid})`);
        // Don't add to blockProcesses or show green dot yet - wait for BLOCK_READY
      } else if (data.type === 'stdout') {
        const output = data.data.trim();
        
        // Parse protocol message
        const protocolMsg = parseProtocolMessage(output);
        
        if (protocolMsg) {
          handleProtocolMessage(protocolMsg, data);
        } else {
          // Non-protocol message - log it
          if (!output.match(/Initializing|Testing|Started/)) {
            addLog('info', `[${data.name || data.pid}]: ${output}`);
          }
        }
      } else if (data.type === 'stderr') {
        addLog('warning', `[${data.name || data.pid}]: ${data.data.trim()}`);
      } else if (data.type === 'exit') {
        handleProcessExit(data);
      } else if (data.type === 'killed') {
        handleProcessKilled(data);
      } else if (data.type === 'error') {
        addLog('error', `${data.name || 'Process'}: ${data.data}`);
      }
    });
    
    return () => {
      if (processOutputListenerRef.current) {
        processOutputListenerRef.current();
      }
    };
  }, [blockProcesses, addLog]);

  const handleProtocolMessage = (msg, data) => {
    const { blockId, msgType, timestamp } = msg;

    switch (msgType) {
      case 'BLOCK_INIT':
        addLog('info', `${msg.data}: Initializing...`);
        setBlockStatus(prev => ({
          ...prev,
          [blockId]: 'initializing'
        }));
        break;

      case 'BLOCK_READY':
        addLog('success', `${msg.data}: Ready`);
        setBlockStatus(prev => ({
          ...prev,
          [blockId]: 'ready'
        }));
        // NOW mark block as running and add to blockProcesses
        setBlockProcesses(prev => ({
          ...prev,
          [blockId]: {
            pid: data.pid,
            status: 'running',
            name: data.name
          }
        }));
        break;

      case 'BLOCK_ERROR':
        addLog('error', `${data.name}: ${msg.data}`);
        setBlockStatus(prev => ({
          ...prev,
          [blockId]: 'error'
        }));
        break;

      case 'BLOCK_METRICS':
        const metrics = parseMetricsData(msg.data);
        if (metrics) {
          setBlockMetrics(prev => ({
            ...prev,
            [blockId]: metrics
          }));
        }
        break;

      case 'BLOCK_GRAPH':
        const graphPoint = parseGraphData(msg.data);
        if (graphPoint) {
          setGraphData(prev => {
            const existing = prev[blockId] || { xData: [], yData: [] };
            const maxPoints = 500;
            
            const newXData = [...existing.xData, graphPoint.x];
            const newYData = [...existing.yData, graphPoint.y];
            
            if (newXData.length > maxPoints) {
              newXData.shift();
              newYData.shift();
            }
            
            return {
              ...prev,
              [blockId]: {
                xData: newXData,
                yData: newYData
              }
            };
          });
        }
        break;

      case 'BLOCK_STOPPING':
        addLog('info', `${msg.data}: Stopping...`);
        setBlockStatus(prev => ({
          ...prev,
          [blockId]: 'stopping'
        }));
        break;

      case 'BLOCK_STOPPED':
        addLog('info', `${msg.data}: Stopped`);
        setBlockStatus(prev => ({
          ...prev,
          [blockId]: 'stopped'
        }));
        break;

      default:
        console.warn('Unknown protocol message type:', msgType);
    }
  };

  const handleProcessExit = (data) => {
    addLog('info', `${data.name || 'Process'}: Exited with code ${data.code}`);
    
    const blockEntry = Object.entries(blockProcesses).find(([key, proc]) => proc.pid === data.pid);
    if (blockEntry) {
      const [key] = blockEntry;
      const blockId = key.startsWith('source_') ? parseInt(key.replace('source_', '')) : parseInt(key);
      
      // Clean up metrics and graph data
      setBlockMetrics(prev => {
        const newMetrics = { ...prev };
        delete newMetrics[blockId];
        return newMetrics;
      });
      
      setGraphData(prev => {
        const newData = { ...prev };
        delete newData[blockId];
        return newData;
      });

      setBlockStatus(prev => {
        const newStatus = { ...prev };
        delete newStatus[blockId];
        return newStatus;
      });
    }
    
    setBlockProcesses(prev => {
      const newProcs = { ...prev };
      Object.keys(newProcs).forEach(key => {
        if (newProcs[key].pid === data.pid) {
          delete newProcs[key];
        }
      });
      return newProcs;
    });
  };

  const handleProcessKilled = (data) => {
    addLog('info', `${data.name || 'Process'}: Terminated`);
    
    const blockEntry = Object.entries(blockProcesses).find(([key, proc]) => proc.pid === data.pid);
    if (blockEntry) {
      const [key] = blockEntry;
      const blockId = key.startsWith('source_') ? parseInt(key.replace('source_', '')) : parseInt(key);
      
      setBlockMetrics(prev => {
        const newMetrics = { ...prev };
        delete newMetrics[blockId];
        return newMetrics;
      });
      
      setGraphData(prev => {
        const newData = { ...prev };
        delete newData[blockId];
        return newData;
      });

      setBlockStatus(prev => {
        const newStatus = { ...prev };
        delete newStatus[blockId];
        return newStatus;
      });
    }
  };

  return {
    blockProcesses,
    setBlockProcesses,
    blockMetrics,
    setBlockMetrics,
    graphData,
    setGraphData,
    blockStatus
  };
};

import React from 'react';
import { FileCode, Trash2 } from 'lucide-react';

export const Sidebar = ({ 
  sidebarMode,
  selectedBlocks,
  blocks,
  connections,
  serverRunning,
  blockProcesses,
  executionLog,
  onEditCode,
  onDelete,
  onBlockColorChange,
  onKillProcess,
  onClearLog
}) => {
  return (
    <div className="w-96 bg-white border-r border-gray-300 flex flex-col flex-shrink-0">
      <div className="p-4 border-b border-gray-300 bg-gradient-to-r from-blue-600 to-purple-600">
        <h2 className="text-xl font-bold text-white">Pipeline System Designer</h2>
        <p className="text-sm text-blue-100 mt-1">MATLAB Block Diagram Editor</p>
      </div>
      
      <div className="flex-1 overflow-auto p-4">
        {sidebarMode === 'config' && selectedBlocks.length > 0 && (
          <div className="space-y-4">
            <div className="flex items-center justify-between">
              <h3 className="font-semibold text-lg text-gray-800">
                {selectedBlocks.length === 1 ? 'Block Configuration' : `${selectedBlocks.length} Blocks Selected`}
              </h3>
              {selectedBlocks.length === 1 && (
                <button onClick={() => onEditCode(selectedBlocks[0])} className="text-blue-600 hover:text-blue-800">
                  <FileCode size={20} />
                </button>
              )}
            </div>
            
            {selectedBlocks.length === 1 && (
              <div className="bg-gray-50 rounded-lg p-4 space-y-3">
                <div>
                  <label className="text-sm font-medium text-gray-700">Name</label>
                  <p className="text-base font-semibold text-gray-900">{selectedBlocks[0].name}</p>
                </div>
                
                <div className="grid grid-cols-2 gap-3">
                  <div>
                    <label className="text-sm font-medium text-gray-700">Inputs</label>
                    <p className="text-base font-semibold text-gray-900">{selectedBlocks[0].inputs}</p>
                  </div>
                  <div>
                    <label className="text-sm font-medium text-gray-700">Outputs</label>
                    <p className="text-base font-semibold text-gray-900">{selectedBlocks[0].outputs}</p>
                  </div>
                </div>
                
                <div>
                  <label className="text-sm font-medium text-gray-700">Color</label>
                  <div className="flex items-center gap-2 mt-1">
                    <div className="w-8 h-8 rounded border-2 border-gray-300" style={{ backgroundColor: selectedBlocks[0].color }} />
                    <input 
                      type="color" 
                      value={selectedBlocks[0].color} 
                      onChange={(e) => onBlockColorChange(selectedBlocks[0], e.target.value)}
                      className="cursor-pointer" 
                    />
                  </div>
                </div>
              </div>
            )}
            
            <button 
              onClick={onDelete} 
              className="w-full bg-red-600 text-white px-4 py-2 rounded-md hover:bg-red-700 font-medium flex items-center justify-center gap-2"
            >
              <Trash2 size={16} />
              Delete {selectedBlocks.length > 1 ? `${selectedBlocks.length} Blocks` : 'Block'} (Del)
            </button>
          </div>
        )}
        
        {(!sidebarMode || sidebarMode !== 'config') && (
          <>
            {/* System Status */}
            <div className="mb-4">
              <h3 className="font-semibold text-lg text-gray-800 mb-2">System Status</h3>
              <div className="bg-gray-50 rounded-lg p-3 space-y-2 text-sm">
                <div className="flex justify-between">
                  <span className="text-gray-700">Blocks:</span>
                  <span className="font-semibold text-gray-900">{blocks.length}</span>
                </div>
                <div className="flex justify-between">
                  <span className="text-gray-700">Connections:</span>
                  <span className="font-semibold text-gray-900">{connections.length}</span>
                </div>
                <div className="flex justify-between">
                  <span className="text-gray-700">Server:</span>
                  <span className={`font-semibold ${serverRunning ? 'text-green-600' : 'text-red-600'}`}>
                    {serverRunning ? 'Running' : 'Stopped'}
                  </span>
                </div>
                <div className="flex justify-between">
                  <span className="text-gray-700">Active Processes:</span>
                  <span className="font-semibold text-gray-900">{Object.keys(blockProcesses).length}</span>
                </div>
              </div>
            </div>

            {/* Active Processes List */}
            {Object.keys(blockProcesses).length > 0 && (
              <div className="mb-4">
                <h3 className="font-semibold text-lg text-gray-800 mb-2">Active Processes</h3>
                <div className="bg-gray-50 rounded-lg p-3 space-y-2 text-xs max-h-40 overflow-y-auto">
                  {Object.entries(blockProcesses).map(([key, proc]) => (
                    <div key={key} className="flex justify-between items-center p-2 bg-white rounded border border-gray-200">
                      <div>
                        <div className="font-semibold text-gray-900">{proc.name}</div>
                        <div className="text-gray-500">PID: {proc.pid}</div>
                      </div>
                      <div className="flex items-center gap-2">
                        <div className={`w-2 h-2 rounded-full ${proc.status === 'running' ? 'bg-green-500' : 'bg-gray-400'}`} />
                        <button
                          onClick={() => onKillProcess(proc.pid, proc.name)}
                          className="text-red-600 hover:text-red-800"
                          title="Kill process"
                        >
                          ✕
                        </button>
                      </div>
                    </div>
                  ))}
                </div>
              </div>
            )}
          </>
        )}
      </div>
      
      {/* Execution Log */}
      <div className="border-t border-gray-300 p-4">
        <div className="flex justify-between items-center mb-2">
          <h3 className="font-semibold text-sm text-gray-800">Execution Log</h3>
          <button
            onClick={onClearLog}
            className="text-xs text-gray-600 hover:text-gray-800"
            title="Clear log"
          >
            Clear
          </button>
        </div>
        <div className="bg-gray-900 rounded-lg p-3 h-64 overflow-y-auto space-y-1 text-[10px] font-mono">
          {executionLog.slice(-50).map((log, idx) => (
            <div key={idx} className={`
              ${log.type === 'error' ? 'text-red-400' : ''}
              ${log.type === 'warning' ? 'text-yellow-400' : ''}
              ${log.type === 'success' ? 'text-green-400' : ''}
              ${log.type === 'info' ? 'text-blue-400' : 'text-gray-400'}
            `}>
              <span className="text-gray-500">[{log.time}]</span> {log.message}
            </div>
          ))}
        </div>
      </div>
    </div>
  );
};

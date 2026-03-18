import React from 'react';
import { Upload, Play, Square, StopCircle, Save, FolderOpen, X, Wifi, WifiOff } from 'lucide-react';

export const Toolbar = ({ 
  onStartServer, 
  onStart, 
  onStop, 
  onTerminate,
  onImport,
  onLoad,
  onSave,
  serverRunning,
  isStartingServer,
  hasProcesses,
  socketConnected
}) => {
  return (
    <div className="bg-white border-b border-gray-300 p-3 flex gap-2 items-center overflow-x-auto">
      <button 
        onClick={onStartServer} 
        disabled={serverRunning || isStartingServer}
        className="px-4 py-2 bg-green-600 text-white rounded-md hover:bg-green-700 font-medium disabled:bg-gray-400 disabled:cursor-not-allowed flex items-center gap-2 whitespace-nowrap"
      >
        <Play size={18} />
        {isStartingServer ? 'Starting...' : 'Start Server'}
      </button>
      
      {/* Socket Status Indicator */}
      {serverRunning && (
        <div className={`px-3 py-2 rounded-md flex items-center gap-2 text-sm font-medium ${
          socketConnected ? 'bg-green-100 text-green-700' : 'bg-yellow-100 text-yellow-700'
        }`}>
          {socketConnected ? (
            <>
              <Wifi size={16} />
              <span>Socket Connected</span>
            </>
          ) : (
            <>
              <WifiOff size={16} />
              <span>Connecting...</span>
            </>
          )}
        </div>
      )}
      
      <div className="w-px h-8 bg-gray-300 mx-1"></div>
      
      <button 
        onClick={onStart} 
        disabled={!serverRunning}
        className="px-4 py-2 bg-blue-600 text-white rounded-md hover:bg-blue-700 font-medium disabled:bg-gray-400 disabled:cursor-not-allowed flex items-center gap-2 whitespace-nowrap"
      >
        <Play size={18} />
        Start
      </button>
      
      <button 
        onClick={onStop} 
        disabled={!serverRunning}
        className="px-4 py-2 bg-orange-600 text-white rounded-md hover:bg-orange-700 font-medium disabled:bg-gray-400 disabled:cursor-not-allowed flex items-center gap-2 whitespace-nowrap"
      >
        <Square size={18} />
        Stop
      </button>
      
      <button 
        onClick={onTerminate} 
        disabled={!hasProcesses}
        className="px-4 py-2 bg-red-600 text-white rounded-md hover:bg-red-700 font-medium disabled:bg-gray-400 disabled:cursor-not-allowed flex items-center gap-2 whitespace-nowrap"
      >
        <X size={18} />
        Terminate
      </button>
      
      <div className="w-px h-8 bg-gray-300 mx-1"></div>
      
      <button 
        onClick={onImport}
        className="px-4 py-2 bg-blue-600 text-white rounded-md hover:bg-blue-700 font-medium flex items-center gap-2 whitespace-nowrap"
      >
        <Upload size={18} />
        Import
      </button>
      
      <button 
        onClick={onLoad}
        className="px-4 py-2 bg-purple-600 text-white rounded-md hover:bg-purple-700 font-medium flex items-center gap-2 whitespace-nowrap"
      >
        <FolderOpen size={18} />
        Load
      </button>
      
      <button 
        onClick={onSave} 
        className="px-4 py-2 bg-indigo-600 text-white rounded-md hover:bg-indigo-700 font-medium flex items-center gap-2 whitespace-nowrap"
      >
        <Save size={18} />
        Save
      </button>
    </div>
  );
};
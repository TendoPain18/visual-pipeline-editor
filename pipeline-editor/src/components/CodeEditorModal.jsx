import React, { useState } from 'react';

export const CodeEditorModal = ({ block, onSave, onClose }) => {
  const [code, setCode] = useState(block.code);

  return (
    <div 
      className="fixed inset-0 bg-black bg-opacity-50 flex items-center justify-center z-50" 
      onClick={onClose}
    >
      <div 
        className="bg-white rounded-lg shadow-2xl w-3/4 h-3/4 flex flex-col" 
        onClick={(e) => e.stopPropagation()}
      >
        <div className="p-4 border-b border-gray-300 bg-gradient-to-r from-blue-600 to-purple-600 rounded-t-lg">
          <h3 className="text-xl font-bold text-white">Edit Block Code</h3>
          <p className="text-sm text-blue-100 mt-1">{block.fileName}</p>
        </div>
        
        <div className="flex-1 p-4 overflow-hidden">
          <textarea 
            value={code} 
            onChange={(e) => setCode(e.target.value)}
            className="w-full h-full font-mono text-sm p-4 border border-gray-300 rounded-md focus:outline-none focus:ring-2 focus:ring-blue-500 resize-none"
            spellCheck={false}
          />
        </div>
        
        <div className="p-4 border-t border-gray-300 flex gap-3 justify-end bg-gray-50 rounded-b-lg">
          <button 
            onClick={onClose}
            className="px-6 py-2 bg-gray-300 text-gray-700 rounded-md hover:bg-gray-400 font-medium"
          >
            Cancel
          </button>
          <button 
            onClick={() => onSave(code)}
            className="px-6 py-2 bg-blue-600 text-white rounded-md hover:bg-blue-700 font-medium"
          >
            Save Changes
          </button>
        </div>
      </div>
    </div>
  );
};

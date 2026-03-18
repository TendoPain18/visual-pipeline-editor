import { useEffect } from 'react';

export const useKeyboardShortcuts = ({
  onUndo,
  onRedo,
  onCopy,
  onPaste,
  onDelete,
  isEditingCode,
  currentConnection,
  setCurrentConnection,
  setMousePosition,
  addLog
}) => {
  useEffect(() => {
    const handleKeyDown = (e) => {
      // Skip if editing code
      if (isEditingCode) return;
      
      // Escape - Cancel connection
      if (e.key === 'Escape') {
        if (currentConnection) {
          setCurrentConnection(null);
          setMousePosition(null);
          addLog('info', 'Connection cancelled');
          e.preventDefault();
          return;
        }
      }
      
      // Ctrl+Z / Cmd+Z - Undo
      if ((e.ctrlKey || e.metaKey) && e.key === 'z' && !e.shiftKey) {
        e.preventDefault();
        onUndo();
      }
      
      // Ctrl+Y / Cmd+Shift+Z - Redo
      if ((e.ctrlKey || e.metaKey) && (e.key === 'y' || (e.key === 'z' && e.shiftKey))) {
        e.preventDefault();
        onRedo();
      }
      
      // Ctrl+C / Cmd+C - Copy
      if ((e.ctrlKey || e.metaKey) && e.key === 'c') {
        e.preventDefault();
        onCopy();
      }
      
      // Ctrl+V / Cmd+V - Paste
      if ((e.ctrlKey || e.metaKey) && e.key === 'v') {
        e.preventDefault();
        onPaste();
      }
      
      // Delete / Backspace - Delete
      if (e.key === 'Delete' || e.key === 'Backspace') {
        e.preventDefault();
        onDelete();
      }
    };

    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [
    onUndo,
    onRedo,
    onCopy,
    onPaste,
    onDelete,
    isEditingCode,
    currentConnection,
    setCurrentConnection,
    setMousePosition,
    addLog
  ]);
};

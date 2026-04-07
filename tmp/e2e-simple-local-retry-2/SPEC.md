# Summary
A command-line interface (CLI) application for managing text-based notes. The application provides functionality to create, list, and delete notes. Data is persisted in a local JSON file named `notes.json`.

# Requirements
- The application provides a `add <text>` command to create a new note.
- Each note contains a unique integer `id`, a `content` string, and a `timestamp` float.
- The application provides a `list` command to display all existing notes.
- The application provides a `remove <id>` command to delete a note by its integer ID.
- The application persists all notes to `notes.json` in the current working directory.
- The application uses integer IDs that increment based on the highest existing ID.

# File Structure
- `src/models.py`
- `src/manager.py`
- `src/main.py`
- `tests/test_manager.py`

# Implementation Details

The application logic is split into data models, a management class for CRUD operations, and a CLI entry point.

```python
# src/models.py
from dataclasses import dataclass

@dataclass
class Note:
    id: int
    content: str
    timestamp: float

# src/manager.py
from typing import List
import json
import os
import time
from src.models import Note

class NoteNotFoundError(Exception):
    """Raised when a requested note ID does not exist."""
    pass

class InvalidIDError(ValueError):
    """Raised when a provided ID is not a positive integer."""
    pass

class NoteManager:
    def __init__(self, storage_path: str) -> None:
        self.storage_path: str = storage_path
        self._ensure_storage_exists()

    def _ensure_storage_exists(self) -> None:
        if not os.path.exists(self.storage_path):
            with open(self.storage_path, 'w') as f:
                json.dump([], f)

    def add_note(self, content: str) -> Note:
        if not content:
            raise ValueError("Content cannot be empty")
        
        notes: List[Note] = self.list_notes()
        new_id: int = max([n.id for n in notes], default=0) + 1
        new_note: Note = Note(id=new_id, content=content, timestamp=time.time())
        
        notes.append(new_note)
        self._save_all(notes)
        return new_note

    def list_notes(self) -> List[Note]:
        with open(self.storage_path, 'r') as f:
            data: List[dict] = json.load(f)
            return [Note(id=n['id'], content=n['content'], timestamp=n['timestamp']) for n in data]

    def remove_note(self, note_id: int) -> None:
        if note_id <= 0:
            raise InvalidIDError(f"ID {note_id} must be a positive integer")
        
        notes: List[Note] = self.list_notes()
        original_count: int = len(notes)
        notes = [n for n in notes if n.id != note_id]
        
        if len(notes) == original_count:
            raise NoteNotFoundError(f"Note with ID {note_id} not found")
        
        self._save_all(notes)

    def _save_all(self, notes: List[Note]) -> None:
        with open(self.storage_path, 'w') as f:
            json.dump([n.__dict__ for n in notes], f)
```

```python
# src/main.py
import sys
from typing import List
from src.manager import NoteManager, NoteNotFoundError, InvalidIDError

def main(args: List[str]) -> None:
    manager: NoteManager = NoteManager("notes.json")
    
    if not args:
        print("Usage: add <text> | list | remove <id>")
        return

    command: str = args[0]

    if command == "add" and len(args) == 2:
        try:
            note = manager.add_note(args[1])
            print(f"Added note {note.id}")
        except ValueError as e:
            print(f"Error: {e}")

    elif command == "list":
        notes = manager.list_notes()
        if not notes:
            print("No notes found.")
        for n in notes:
            print(f"[{n.id}] {n.content}")

    elif command == "remove" and len(args) == 2:
        try:
            note_id = int(args[1])
            manager.remove_note(note_id)
            print(f"Removed note {note_id}")
        except ValueError as e:
            if "invalid literal for int()" in str(e):
                print("Error: ID must be an integer")
            else:
                print(f"Error: {e}")
        except (NoteNotFoundError, InvalidIDError) as e:
            print(f"Error: {e}")
    else:
        print("Invalid command or arguments")

if __name__ == "__main__":
    main(sys.argv[1:])
```

# Testing Strategy
Unit tests will be implemented in `tests/test_manager.py` using `pytest`.

1. **Test Add**: Call `manager.add_note("test")` and verify the returned `Note` object has the correct content and a positive ID.
2. **Test List**: Call `manager.add_note("a")`, `manager.add_note("b")`, then `manager.list_notes()` and verify the list length is 2.
3. **Test Remove Success**: Call `manager.add_note("test")`, then `manager.remove_note(1)` and verify `list_notes()` returns an empty list.
4. **Test Remove Failure**: Call `manager.remove_note(99)` and verify `NoteNotFoundError` is raised.

# Validation Criteria
The following CLI commands must produce the specified outputs:

- `python src/main.py add "First Note"` 
  - Output: `Added note 1`
- `python src/main.py list`
  - Output: `[1] First Note`
- `python src/main.py remove 1`
  - Output: `Removed note 1`
- `python src/main.py remove 1` (after removal)
  - Output: `Error: Note with ID 1 not found`
- `python src/main.py remove -5`
  - Output: `Error: ID -5 must be a positive integer`

# Error Handling And Edge Cases

### Error Behavior
- `NoteNotFoundError`: Raised when `remove_note` is called with an ID that does not exist in the JSON storage.
- `InvalidIDError`: Raised when `remove_note` is called with an integer less than or equal to 0.
- `ValueError`: Raised when `add_note` is called with an empty string `""`.
- `json.JSONDecodeError`: Raised if `notes.json` contains invalid JSON syntax.

### Edge Cases
- **Empty Input (Add)**: Calling `add_note("")` results in a `ValueError`.
- **None-like Input**: The CLI implementation treats missing arguments as a usage error.
- **Empty Storage**: If `notes.json` is empty or contains `[]`, `list_notes()` returns an empty list `[]`.
- **Non-Integer ID**: Calling `remove abc` results in a printed error: `Error: ID must be an integer`.
- **Boundary ID**: Calling `remove 0` results in `InvalidIDError`.
- **Missing File**: If `notes.json` does not exist, the `NoteManager` creates a new file containing `[]`.
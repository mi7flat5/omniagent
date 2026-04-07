1. # Summary  
A Python command-line interface (CLI) todo application that supports four commands: `add`, `list`, `complete`, and `remove`. The application persists todo items in a JSON file (`todos.json`) in the current working directory. All operations are deterministic and validated against strict input constraints.

2. # Requirements  
- The CLI must accept exactly four subcommands: `add`, `list`, `complete`, `remove`.  
- `add <description>`: Adds a new todo item with the given description. Returns the created item ID.  
- `list`: Lists all todo items in the order they were added.  
- `complete <id>`: Marks the todo item with the given ID as completed.  
- `remove <id>`: Removes the todo item with the given ID.  
- All IDs are non-negative integers starting from 0 and incrementing sequentially.  
- The JSON file `todos.json` must be created if it does not exist.  
- The JSON file must contain a list of todo objects, each with fields: `id` (int), `description` (str), `completed` (bool).  
- The application must validate that `id` arguments are non-negative integers.  
- The application must raise a `ValueError` for invalid IDs or missing items.  
- The application must raise a `FileNotFoundError` if the JSON file is missing during `list`, `complete`, or `remove` operations.  

3. # File Structure  
```
src/
├── __init__.py
├── cli.py
├── storage.py
└── models.py
tests/
├── __init__.py
├── test_cli.py
├── test_storage.py
└── test_models.py
todos.json
```

4. # Implementation Details  
The application uses three core modules: `models.py` for data structures, `storage.py` for file I/O, and `cli.py` for command parsing and dispatch.

```python
# src/models.py
from dataclasses import dataclass
from typing import List

@dataclass
class Todo:
    id: int
    description: str
    completed: bool = False

def create_todo(description: str, todo_id: int) -> Todo:
    return Todo(id=todo_id, description=description, completed=False)

def complete_todo(todo: Todo) -> Todo:
    return Todo(id=todo.id, description=todo.description, completed=True)

def remove_todo(todo: Todo) -> None:
    pass  # No-op; removal is handled by storage layer
```

```python
# src/storage.py
import json
import os
from typing import List, Optional
from src.models import Todo

TODOS_FILE = "todos.json"

def load_todos() -> List[Todo]:
    if not os.path.exists(TODOS_FILE):
        raise FileNotFoundError(f"Todos file '{TODOS_FILE}' not found")
    with open(TODOS_FILE, "r", encoding="utf-8") as f:
        data = json.load(f)
    return [Todo(id=item["id"], description=item["description"], completed=item["completed"]) for item in data]

def save_todos(todos: List[Todo]) -> None:
    with open(TODOS_FILE, "w", encoding="utf-8") as f:
        json.dump([{"id": t.id, "description": t.description, "completed": t.completed} for t in todos], f, indent=2)

def add_todo(description: str) -> int:
    todos = load_todos() if os.path.exists(TODOS_FILE) else []
    new_id = len(todos)
    new_todo = Todo(id=new_id, description=description, completed=False)
    todos.append(new_todo)
    save_todos(todos)
    return new_id

def get_todo_by_id(todo_id: int) -> Todo:
    todos = load_todos()
    for todo in todos:
        if todo.id == todo_id:
            return todo
    raise ValueError(f"Todo with id {todo_id} not found")

def update_todo(todo: Todo) -> None:
    todos = load_todos()
    for i, t in enumerate(todos):
        if t.id == todo.id:
            todos[i] = todo
            break
    else:
        raise ValueError(f"Todo with id {todo.id} not found")
    save_todos(todos)

def remove_todo_by_id(todo_id: int) -> None:
    todos = load_todos()
    for i, t in enumerate(todos):
        if t.id == todo_id:
            del todos[i]
            break
    else:
        raise ValueError(f"Todo with id {todo_id} not found")
    save_todos(todos)
```

5. # Testing Strategy  
Unit tests for each module verify deterministic behavior under valid and invalid inputs.

```python
# tests/test_storage.py
import os
import pytest
from src.storage import load_todos, add_todo, get_todo_by_id, update_todo, remove_todo_by_id, save_todos
from src.models import Todo

def test_add_todo_creates_file_and_returns_id(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    todo_id = add_todo("Buy milk")
    assert todo_id == 0
    assert os.path.exists("todos.json")

def test_get_todo_by_id_raises_value_error_for_missing_id(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    with pytest.raises(ValueError, match="Todo with id 999 not found"):
        get_todo_by_id(999)

def test_remove_todo_by_id_removes_item(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    add_todo("Task A")
    add_todo("Task B")
    remove_todo_by_id(0)
    todos = load_todos()
    assert len(todos) == 1
    assert todos[0].id == 1
```

6. # Validation Criteria  
The CLI must produce deterministic output for all commands. The following usage examples must execute without error:

```python
# tests/test_cli.py
import subprocess
import json
import os

def test_cli_add_command(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    result = subprocess.run(["python", "-m", "src.cli", "add", "Test task"], capture_output=True, text=True)
    assert result.returncode == 0
    assert "0" in result.stdout.strip()
    with open("todos.json") as f:
        data = json.load(f)
    assert data == [{"id": 0, "description": "Test task", "completed": False}]

def test_cli_complete_command(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    subprocess.run(["python", "-m", "src.cli", "add", "Task 1"], capture_output=True)
    result = subprocess.run(["python", "-m", "src.cli", "complete", "0"], capture_output=True, text=True)
    assert result.returncode == 0
    with open("todos.json") as f:
        data = json.load(f)
    assert data[0]["completed"] is True

def test_cli_remove_command(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    subprocess.run(["python", "-m", "src.cli", "add", "Task 1"], capture_output=True)
    result = subprocess.run(["python", "-m", "src.cli", "remove", "0"], capture_output=True, text=True)
    assert result.returncode == 0
    with open("todos.json") as f:
        data = json.load(f)
    assert data == []
```

7. # Error Handling And Edge Cases  
- `add_todo("")`: Raises `ValueError("Description must not be empty")`.  
- `get_todo_by_id(-1)`: Raises `ValueError("ID must be non-negative")`.  
- `complete_todo_by_id(0)` when `todos.json` does not exist: Raises `FileNotFoundError("Todos file 'todos.json' not found")`.  
- `remove_todo_by_id(0)` when no todo with ID 0 exists: Raises `ValueError("Todo with id 0 not found")`.  
- `list` command when `todos.json` does not exist: Returns an empty list `[]`.  
- `complete` or `remove` with non-integer ID: Raises `ValueError("ID must be a non-negative integer")`.  
- `add_todo("  ")` (whitespace-only description): Treated as invalid and raises `ValueError("Description must not be empty")`.
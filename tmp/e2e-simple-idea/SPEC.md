# Summary

The Todo CLI application is a command-line interface tool for managing personal tasks. The application stores task data in a local JSON file. Users interact with the system via four primary commands: add, list, complete, and remove. The system assigns unique integer IDs to tasks upon creation. The application runs on Python 3.9 or higher.

# Requirements

1. The application must persist todo items to a JSON file located at `data/todos.json`.
2. The application must support adding a new todo item with a text description.
3. The application must support listing all todo items with their ID, description, and completion status.
4. The application must support marking a todo item as complete by ID.
5. The application must support removing a todo item by ID.
6. The application must assign sequential integer IDs starting from 1.
7. The application must load existing data from the JSON file on startup.
8. The application must save data to the JSON file after every modification.
9. The application must exit with status code 0 on success.
10. The application must exit with status code 1 on error.

# File Structure

- `src/todo_app/__init__.py`
- `src/todo_app/main.py`
- `src/todo_app/storage.py`
- `src/todo_app/exceptions.py`
- `tests/__init__.py`
- `tests/test_storage.py`
- `tests/test_main.py`
- `data/todos.json`

# Implementation Details

The `storage.py` module handles all file I/O operations. It loads data from the JSON file into a list of dictionaries and saves the list back to the file. The `main.py` module contains the business logic for manipulating todo items and calls storage functions. The `exceptions.py` module defines custom exceptions for error handling.

```python
# src/todo_app/storage.py
import json
from pathlib import Path
from typing import List, Dict, Any

DEFAULT_STORAGE_PATH: Path = Path("data/todos.json")

def load_todos(file_path: Path = DEFAULT_STORAGE_PATH) -> List[Dict[str, Any]]:
    if not file_path.exists():
        return []
    with open(file_path, "r", encoding="utf-8") as file_handle:
        content: str = file_handle.read()
        if not content.strip():
            return []
        data: Any = json.loads(content)
        if not isinstance(data, list):
            return []
        return data

def save_todos(todos: List[Dict[str, Any]], file_path: Path = DEFAULT_STORAGE_PATH) -> None:
    file_path.parent.mkdir(parents=True, exist_ok=True)
    with open(file_path, "w", encoding="utf-8") as file_handle:
        json.dump(todos, file_handle, indent=2)
```

```python
# src/todo_app/main.py
from typing import List, Dict, Any
from todo_app.storage import load_todos, save_todos
from todo_app.exceptions import TodoItemNotFoundError

def add_todo(title: str) -> int:
    todos: List[Dict[str, Any]] = load_todos()
    new_id: int = 1
    if todos:
        max_id: int = max(item["id"] for item in todos)
        new_id = max_id + 1
    new_item: Dict[str, Any] = {"id": new_id, "title": title, "completed": False}
    todos.append(new_item)
    save_todos(todos)
    return new_id

def list_todos() -> List[Dict[str, Any]]:
    todos: List[Dict[str, Any]] = load_todos()
    return todos

def complete_todo(todo_id: int) -> None:
    todos: List[Dict[str, Any]] = load_todos()
    found: bool = False
    for item in todos:
        if item["id"] == todo_id:
            item["completed"] = True
            found = True
            break
    if not found:
        raise TodoItemNotFoundError(f"Todo item with ID {todo_id} not found")
    save_todos(todos)

def remove_todo(todo_id: int) -> None:
    todos: List[Dict[str, Any]] = load_todos()
    initial_length: int = len(todos)
    todos[:] = [item for item in todos if item["id"] != todo_id]
    if len(todos) == initial_length:
        raise TodoItemNotFoundError(f"Todo item with ID {todo_id} not found")
    save_todos(todos)
```

# Testing Strategy

Tests use the `pytest` framework. Tests mock the file system to prevent modification of the actual `data/todos.json` file during execution. Unit tests cover each public function in `main.py` and `storage.py`.

1. `test_storage.py` validates `load_todos` returns an empty list for missing files.
2. `test_storage.py` validates `save_todos` creates the parent directory if it does not exist.
3. `test_main.py` validates `add_todo` returns the correct ID.
4. `test_main.py` validates `complete_todo` raises `TodoItemNotFoundError` for invalid IDs.
5. `test_main.py` validates `remove_todo` decreases the list length by 1.

Concrete usage examples for test validation:

```python
from todo_app.main import add_todo, list_todos
from todo_app.storage import load_todos

# Test add functionality
task_id: int = add_todo("Write specification")
assert task_id == 1

# Test list functionality
items: list = list_todos()
assert len(items) == 1
assert items[0]["title"] == "Write specification"
```

# Validation Criteria

Validation requires executing the CLI commands and verifying the state of `data/todos.json`.

1. Execute `python -m todo_app add "Buy milk"`. Verify `data/todos.json` contains one item with title "Buy milk" and ID 1.
2. Execute `python -m todo_app list`. Verify stdout displays the item added in step 1.
3. Execute `python -m todo_app complete 1`. Verify `data/todos.json` shows `"completed": true` for ID 1.
4. Execute `python -m todo_app remove 1`. Verify `data/todos.json` contains an empty list.
5. Execute `python -m todo_app complete 999`. Verify the process exits with status code 1 and prints an error message.

Programmatic validation calls:

```python
from todo_app.main import add_todo, complete_todo, remove_todo, list_todos

# Validate workflow
id_1: int = add_todo("Task A")
assert id_1 == 1
complete_todo(id_1)
remove_todo(id_1)
remaining: list = list_todos()
assert len(remaining) == 0
```

# Error Handling And Edge Cases

The application defines custom exceptions in `src/todo_app/exceptions.py`.

```python
# src/todo_app/exceptions.py
class TodoItemNotFoundError(Exception):
    pass

class TodoStorageError(Exception):
    pass
```

1. **Empty Input**: If the JSON file is empty or contains only whitespace, `load_todos` returns an empty list.
2. **None Input**: Functions do not accept `None` for required string parameters. Passing `None` to `add_todo` raises `TypeError`.
3. **Negative ID**: Passing a negative integer to `complete_todo` or `remove_todo` raises `TodoItemNotFoundError`.
4. **Zero ID**: Passing 0 to `complete_todo` or `remove_todo` raises `TodoItemNotFoundError`.
5. **Non-existent ID**: Passing an ID that does not exist in the list to `complete_todo` or `remove_todo` raises `TodoItemNotFoundError`.
6. **Empty Title**: Passing an empty string to `add_todo` creates a task with an empty title.
7. **Corrupted JSON**: If `data/todos.json` contains invalid JSON, `load_todos` raises `json.JSONDecodeError`.
8. **Permission Error**: If the application lacks write permission for `data/todos.json`, `save_todos` raises `PermissionError`.
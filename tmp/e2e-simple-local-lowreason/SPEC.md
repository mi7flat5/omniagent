# Summary
A command-line interface (CLI) application for managing a task list. The application provides functionality to create, list, mark as complete, and delete tasks. Data is persisted in a local JSON file.

# Requirements
1. The application must provide a CLI interface via the `argparse` module.
2. The `add` command must accept a string argument and append a new task to the list.
3. The `list` command must display all tasks, showing their unique integer ID, description, and completion status.
4. The `complete` command must accept an integer ID and set the `completed` status of that task to `True`.
5. The `remove` command must accept an integer ID and delete the task from the list.
6. The application must persist all tasks to a file named `tasks.json` in the current working directory.
7. The application must assign auto-incrementing integer IDs to tasks.

# File Structure
src/
  todo_app/
    __init__.py
    cli.py
    core.py
    exceptions.py
    models.py
tests/
  test_core.py
tasks.json

# Implementation Details

The application logic is divided into data models, custom exceptions, and a management class.

```python
# src/todo_app/models.py
from dataclasses import dataclass

@dataclass
class Task:
    id: int
    description: str
    completed: bool
```

```python
# src/todo_app/exceptions.py
class TodoError(Exception):
    """Base exception for the todo application."""
    pass

class TaskNotFoundError(TodoError):
    """Raised when a requested task ID does not exist."""
    pass

class StorageError(TodoError):
    """Raised when JSON file operations fail."""
    pass
```

```python
# src/todo_app/core.py
import json
import os
from typing import List, Dict
from todo_app.models import Task
from todo_app.exceptions import TaskNotFoundError, StorageError

class TodoManager:
    def __init__(self, file_path: str) -> None:
        self.file_path: str = file_path
        self.tasks: List[Task] = self._load_tasks()

    def _load_tasks(self) -> List[Task]:
        if not os.path.exists(self.file_path):
            return []
        try:
            with open(self.file_path, 'r') as f:
                data: List[Dict] = json.load(f)
                return [Task(id=t['id'], description=t['description'], completed=t['completed']) for t in data]
        except (json.JSONDecodeError, KeyError, TypeError) as e:
            raise StorageError(f"Data corruption detected: {e}")

    def _save_tasks(self) -> None:
        try:
            with open(self.file_path, 'w') as f:
                json.dump([vars(t) for t in self.tasks], f)
        except Exception as e:
            raise StorageError(f"Failed to write to file: {e}")

    def add_task(self, description: str) -> Task:
        if not description or description.strip() == "":
            raise ValueError("Description cannot be empty")
        
        new_id: int = max([t.id for t in self.tasks], default=0) + 1
        new_task: Task = Task(id=new_id, description=description, completed=False)
        self.tasks.append(new_task)
        self._save_tasks()
        return new_task

    def complete_task(self, task_id: int) -> None:
        for task in self.tasks:
            if task.id == task_id:
                task.completed = True
                self._save_tasks()
                return
        raise TaskNotFoundError(f"Task with ID {task_id} not found")

    def remove_task(self, task_id: int) -> None:
        initial_length: int = len(self.tasks)
        self.tasks = [t for t in self.tasks if t.id != task_id]
        if len(self.tasks) == initial_length:
            raise TaskNotFoundError(f"Task with ID {task_id} not found")
        self._save_tasks()
```

# Testing Strategy
Tests will be implemented using `pytest`.

1. **Unit Tests for `TodoManager`**:
    - `test_add_task_success`: Verify `manager.add_task("test")` returns a `Task` object with `completed=False`.
    - `test_add_task_empty_description`: Verify `manager.add_task("")` raises `ValueError`.
    - `test_complete_task_success`: Verify `manager.complete_task(1)` updates the task status.
    - `test_complete_task_not_found`: Verify `manager.complete_task(999)` raises `TaskNotFoundError`.
    - `test_remove_task_success`: Verify `manager.remove_task(1)` reduces the list length.
    - `test_remove_task_not_found`: Verify `manager.remove_task(999)` raises `TaskNotFoundError`.
2. **Persistence Tests**:
    - Verify that after `manager.add_task("persist")`, the `tasks.json` file contains the expected JSON structure.

# Validation Criteria
1. `manager.add_task("Task 1")` must return a `Task` instance where `task.id == 1`.
2. `manager.complete_task(1)` must result in `manager.tasks[0].completed == True`.
3. `manager.remove_task(1)` must result in `len(manager.tasks) == 0` if only one task existed.
4. The CLI must exit with a non-zero status code when `TaskNotFoundError` is raised.

# Error Handling And Edge Cases
1. **Empty Input**: If `add_task` receives an empty string or a string containing only whitespace, it must raise `ValueError`.
2. **None Input**: If `add_task` receives `None`, it must raise `ValueError`.
3. **Non-existent ID**: If `complete_task` or `remove_task` is called with an ID that does not exist in the current task list, the system must raise `TaskNotFoundError`.
4. **Zero/Negative ID**: If `complete_task` or `remove_task` is called with an ID $\le 0$, the system must raise `TaskNotFoundError`.
5. **Missing File**: If `tasks.json` does not exist, the system must initialize an empty list and create the file upon the first `add_task` call.
6. **Corrupted JSON**: If `tasks.json` contains invalid JSON or missing required keys (`id`, `description`, `completed`), the system must raise `StorageError`.
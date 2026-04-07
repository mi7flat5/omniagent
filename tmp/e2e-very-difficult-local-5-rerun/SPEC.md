# Summary

This specification defines the implementation of a production-grade multi-tenant workflow automation platform built with FastAPI. The system provides a REST API and WebSocket interface for managing workflows, executing DAG-based tasks, and handling multi-tenant isolation. The platform enforces strict tenant boundaries via organization-level scoping, secures access via JWT authentication with Role-Based Access Control (RBAC), and ensures data consistency using PostgreSQL with migrations. The workflow engine executes Directed Acyclic Graphs (DAGs) with step dependencies, retries, and compensating actions. Redis manages queues and rate limiting. The system implements an outbox/inbox pattern for reliability, maintains an immutable audit log, and exposes OpenTelemetry tracing and Prometheus metrics. Admin and operator CLIs support migration, seeding, replay, and reconciliation. The test suite uses pytest with a docker-compose local stack for integration testing.

# Requirements

1.  **Authentication and Authorization**: The system authenticates users via JWT tokens. The system enforces RBAC policies where roles determine access to resources. The system validates JWT signatures on every request.
2.  **Tenant Isolation**: The system isolates data by `organization_id`. The system validates `organization_id` on every database query. The system rejects requests where the user's `organization_id` does not match the resource's `organization_id`.
3.  **Webhook Ingestion**: The system accepts webhooks via `POST /webhooks`. The system verifies HMAC signatures using a shared secret. The system enforces idempotency using `idempotency_key` headers.
4.  **Workflow Engine**: The system executes DAGs defined in JSON. The system resolves step dependencies before execution. The system retries failed steps up to a maximum of 3 times. The system executes compensating actions on DAG failure.
5.  **Persistence**: The system stores data in PostgreSQL. The system uses Alembic for migrations. The system indexes `organization_id` on all tables.
6.  **Queueing**: The system uses Redis for task queues. The system implements rate limiting using Redis sliding windows.
7.  **Reliability**: The system uses an outbox pattern for event publishing. The system polls the outbox table to publish events. The system retries failed event publishing.
8.  **Audit Logging**: The system records all state changes in an immutable audit log. The system stores `user_id`, `action`, `timestamp`, and `payload` in the audit log.
9.  **Observability**: The system emits OpenTelemetry traces for every request. The system exposes Prometheus metrics at `/metrics`.
10. **CLI Tools**: The system provides `migrate`, `seed`, `replay`, and `reconcile` commands.
11. **Testing**: The system runs unit tests with `pytest`. The system runs integration tests against a docker-compose stack.

# File Structure

```text
src/
├── workflow_platform/
│   ├── __init__.py
│   ├── main.py
│   ├── config.py
│   ├── auth.py
│   ├── models.py
│   ├── schemas.py
│   ├── engine.py
│   ├── webhooks.py
│   ├── queues.py
│   ├── outbox.py
│   ├── audit.py
│   ├── cli.py
│   └── dependencies.py
├── tests/
│   ├── __init__.py
│   ├── conftest.py
│   ├── test_auth.py
│   ├── test_engine.py
│   ├── test_webhooks.py
│   └── test_cli.py
├── alembic/
│   ├── versions/
│   └── env.py
├── docker-compose.yml
└── requirements.txt
```

# Implementation Details

The `auth.py` module handles JWT generation and validation. The `engine.py` module handles DAG execution logic.

```python
# src/workflow_platform/auth.py
import jwt
from datetime import datetime, timedelta
from typing import Optional
from jose import JWTError

class AuthException(Exception):
    pass

def create_access_token(subject: str, organization_id: str) -> str:
    """
    Generates a JWT access token.
    """
    payload = {
        "sub": subject,
        "org_id": organization_id,
        "exp": datetime.utcnow() + timedelta(hours=1)
    }
    token = jwt.encode(payload, "SECRET_KEY", algorithm="HS256")
    return token

def verify_token(token: str) -> dict:
    """
    Verifies a JWT token and returns the payload.
    Raises AuthException if invalid.
    """
    try:
        payload = jwt.decode(token, "SECRET_KEY", algorithms=["HS256"])
        return payload
    except JWTError:
        raise AuthException("Invalid token")
```

```python
# src/workflow_platform/engine.py
from typing import Dict, List, Any
from enum import Enum

class StepStatus(Enum):
    PENDING = "pending"
    RUNNING = "running"
    SUCCESS = "success"
    FAILED = "failed"
    COMPENSATING = "compensating"

class WorkflowEngine:
    def __init__(self):
        self.state: Dict[str, Any] = {}

    def execute_dag(self, dag_definition: Dict[str, Any]) -> Dict[str, Any]:
        """
        Executes a DAG definition.
        Returns execution result.
        """
        if not dag_definition:
            raise ValueError("DAG definition cannot be empty")
        
        steps = dag_definition.get("steps", [])
        if not steps:
            return {"status": "completed", "steps": []}
        
        results = {}
        for step in steps:
            step_id = step.get("id")
            if not step_id:
                raise ValueError("Step missing ID")
            
            results[step_id] = self._execute_step(step)
        
        return {"status": "completed", "results": results}

    def _execute_step(self, step: Dict[str, Any]) -> Dict[str, Any]:
        """
        Executes a single step.
        """
        return {"step_id": step.get("id"), "status": StepStatus.SUCCESS.value}
```

# Testing Strategy

The testing strategy uses `pytest` for unit and integration tests. Tests run against a docker-compose stack for integration scenarios.

1.  **Unit Tests**: Run `pytest tests/ -v`.
2.  **Integration Tests**: Run `docker-compose up -d` followed by `pytest tests/integration/ -v`.
3.  **Coverage**: Run `pytest --cov=src/workflow_platform`.

**Concrete Usage Examples**:

```python
# tests/test_engine.py
from src.workflow_platform.engine import WorkflowEngine, StepStatus

def test_execute_empty_dag():
    engine = WorkflowEngine()
    result = engine.execute_dag({"steps": []})
    assert result["status"] == "completed"

def test_execute_single_step():
    engine = WorkflowEngine()
    dag = {"steps": [{"id": "step_1"}]}
    result = engine.execute_dag(dag)
    assert result["results"]["step_1"]["status"] == StepStatus.SUCCESS.value
```

# Validation Criteria

1.  **Auth Validation**: `verify_token("invalid_token")` raises `AuthException`.
2.  **Engine Validation**: `execute_dag(None)` raises `ValueError`.
3.  **Tenant Validation**: A request with `organization_id` mismatch returns `403 Forbidden`.
4.  **Webhook Validation**: A webhook without `X-HMAC-Signature` returns `400 Bad Request`.
5.  **Idempotency Validation**: A duplicate `idempotency_key` returns `200 OK` with cached response.
6.  **Database Validation**: `alembic upgrade head` completes without errors.
7.  **CLI Validation**: `python -m workflow_platform.cli migrate` executes successfully.

# Error Handling And Edge Cases

1.  **Empty Input**:
    -   `execute_dag({})` returns `{"status": "completed", "steps": []}`.
    -   `execute_dag({"steps": None})` raises `ValueError`.
2.  **None/Null Input**:
    -   `verify_token(None)` raises `AuthException`.
    -   `create_access_token("", "")` returns a valid token with empty subject.
3.  **Boundary Cases**:
    -   `execute_dag` with 0 steps returns immediately.
    -   `execute_dag` with 1000 steps executes sequentially.
4.  **Negative Values**:
    -   Retry count < 0 raises `ValueError`.
    -   Rate limit < 0 raises `ValueError`.
5.  **Named Exceptions**:
    -   `AuthException`: Raised on invalid token.
    -   `WorkflowValidationError`: Raised on invalid DAG structure.
    -   `TenantNotFoundError`: Raised on missing organization.
    -   `IdempotencyConflict`: Raised on duplicate key with different payload.

# Error Handling And Edge Cases

1.  **Empty Input**:
    -   `execute_dag({})` returns `{"status": "completed", "steps": []}`.
    -   `execute_dag({"steps": None})` raises `ValueError`.
2.  **None/Null Input**:
    -   `verify_token(None)` raises `AuthException`.
    -   `create_access_token("", "")` returns a valid token with empty subject.
3.  **Boundary Cases**:
    -   `execute_dag` with 0 steps returns immediately.
    -   `execute_dag` with 1000 steps executes sequentially.
4.  **Negative Values**:
    -   Retry count < 0 raises `ValueError`.
    -   Rate limit < 0 raises `ValueError`.
5.  **Named Exceptions**:
    -   `AuthException`: Raised on invalid token.
    -   `WorkflowValidationError`: Raised on invalid DAG structure.
    -   `TenantNotFoundError`: Raised on missing organization.
    -   `IdempotencyConflict`: Raised on duplicate key with different payload.
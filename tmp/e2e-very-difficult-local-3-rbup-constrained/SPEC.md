# Summary

This specification defines the implementation of a production-grade, multi-tenant workflow automation platform. The system provides a DAG-based workflow engine, robust webhook ingestion with security verification, and strict tenant isolation using PostgreSQL and Redis. The platform implements the Outbox/Inbox pattern to ensure reliable event delivery and maintains an immutable audit log for all system actions.

# Requirements

## Core Functional Requirements
- **Multi-Tenancy**: Organization-level isolation for all data, including users, workflows, and execution history.
- **Authentication & RBAC**: JWT-based authentication with Role-Based Access Control (Admin, Operator, User).
- **Workflow Engine**: Directed Acyclic Graph (DAG) execution supporting step dependencies, retries with exponential backoff, and compensating actions for failure recovery.
- **Webhook Ingestion**: Secure endpoint for external triggers using HMAC signature verification and idempotency keys to prevent duplicate processing.
- **Reliability Patterns**: Implementation of the Outbox pattern for database-to-queue consistency and the Inbox pattern for idempotent message processing.
- **Observability**: OpenTelemetry integration for distributed tracing and Prometheus metrics for system health and performance.
- **CLI Tools**: Administrative interfaces for database migrations, data seeding, event replaying, and state reconciliation.

## Non-Functional Requirements
- **Persistence**: PostgreSQL for relational data and event history.
- **Concurrency & Rate Limiting**: Redis-backed distributed queues and sliding-window rate limiting.
- **Auditability**: Immutable event log capturing all state changes.

# File Structure

```text
.
├── src/
│   ├── __init__.py
│   ├── api/
│   │   ├── __init__.py
│   │   ├── auth.py
│   │   ├── webhooks.py
│   │   └── routes.py
│   ├── core/
│   │   ├── __init__.py
│   │   ├── exceptions.py
│   │   ├── security.py
│   │   └── tenant.py
│   ├── engine/
│   │   ├── __init__.py
│   │   ├── dag.py
│   │   ├── executor.py
│   │   └── models.py
│   ├── infrastructure/
│   │   ├── __init__.py
│   │   ├── database.py
│   │   ├── queue.py
│   │   └── redis.py
│   └── workers/
│       ├── __init__.py
│       └── outbox_worker.py
├── tests/
│   ├── __init__.py
│   ├── test_api_auth.py
│   ├── test_api_webhooks.py
│   ├── test_core_tenant.py
│   ├── test_engine_dag.py
│   ├── test_infrastructure_db.py
│   └── test_workers_outbox.py
├── migrations/
├── scripts/
│   ├── migrate.py
│   └── seed.py
├── docker-compose.yml
└── pyproject.toml
```

# Implementation Details

## Domain Models and Exceptions

```python
from dataclasses import dataclass
from datetime import datetime
from enum import Enum
from typing import Any, Dict, List, Optional
from uuid import UUID

class WorkflowError(Exception):
    """Base exception for all workflow related errors."""
    pass

class TenantAccessDeniedError(WorkflowError):
    """Raised when a tenant attempts to access resources outside their scope."""
    pass

class IdempotencyConflictError(WorkflowError):
    """Raised when a duplicate idempotency key is detected."""
    pass

class StepExecutionError(WorkflowError):
    """Raised when a specific DAG step fails after all retries."""
    pass

@dataclass(frozen=True)
class TenantContext:
    tenant_id: UUID
    organization_id: UUID
    role: str
```

## Workflow Engine Implementation

```python
from typing import Dict, List, Protocol
from uuid import UUID
from src.core.exceptions import StepExecutionError

class Step(Protocol):
    def execute(self, context: Dict[str, Any]) -> Dict[str, Any]: ...
    def compensate(self, context: Dict[str, Any]) -> None: ...

class DAGExecutor:
    def __init__(self, tenant_id: UUID, max_retries: int = 3):
        self.tenant_id: UUID = tenant_id
        self.max_retries: int = max_retries

    def run_step_with_retry(self, step: Step, context: Dict[str, Any]) -> Dict[str, Any]:
        attempts: int = 0
        last_exception: Optional[Exception] = None
        
        while attempts < self.max_retries:
            try:
                return step.execute(context)
            except Exception as e:
                attempts += 1
                last_exception = e
                if attempts >= self.max_retries:
                    break
        
        raise StepExecutionError(f"Step failed after {self.max_retries} attempts. Last error: {last_exception}")

    def execute_workflow(self, dag_nodes: List[Step], initial_context: Dict[str, Any]) -> Dict[str, Any]:
        current_context: Dict[str, Any] = initial_context
        executed_steps: List[Step] = []
        
        try:
            for node in dag_nodes:
                result: Dict[str, Any] = self.run_step_with_retry(node, current_context)
                current_context.update(result)
                executed_steps.append(node)
            return current_context
        except Exception:
            for step in reversed(executed_steps):
                step.compensate(current_context)
            raise
```

## Webhook Ingestion with HMAC

```python
import hmac
import hashlib
from fastapi import Request, HTTPException, Header
from src.core.exceptions import IdempotencyConflictError

class WebhookHandler:
    def __init__(self, secret_key: str, redis_client: Any):
        self.secret_key: str = secret_key
        self.redis: Any = redis_client

    async def verify_signature(self, payload: bytes, signature: str) -> bool:
        expected_signature: str = hmac.new(
            self.secret_key.encode(),
            payload,
            hashlib.sha256
        ).hexdigest()
        return hmac.compare_digest(expected_signature, signature)

    async def process_webhook(self, request: Request, x_signature: str, x_idempotency_key: str) -> Dict[str, Any]:
        body: bytes = await request.body()
        
        if not await self.verify_signature(body, x_signature):
            raise HTTPException(status_code=401, detail="Invalid HMAC signature")

        # Check idempotency in Redis
        is_new: bool = await self.redis.set(
            f"idempotency:{x_idempotency_key}", 
            "processed", 
            nx=True, 
            ex=86400
        )
        if not is_new:
            raise IdempotencyConflictError(f"Key {x_idempotency_key} already processed")

        return {"status": "accepted"}
```

# Testing Strategy

## Unit Tests
- `test_engine_dag.py`: Validates `DAGExecutor.execute_workflow` with successful paths, retry exhaustion, and compensation logic.
- `test_api_webhooks.py`: Validates `WebhookHandler.verify_signature` with valid/invalid signatures and `process_webhook` with duplicate keys.
- `test_core_tenant.py`: Validates `TenantContext` creation and RBAC enforcement.

## Integration Tests
- `test_infrastructure_db.py`: Validates PostgreSQL connection, schema migrations, and Outbox table insertion.
- `test_api_auth.py`: Validates the full JWT flow: `POST /api/auth/login` -> `GET /api/auth/me` with Bearer token.

## Example Test Case
```python
import pytest
from uuid import uuid4
from src.engine.executor import DAGExecutor, StepExecutionError

class MockStep:
    def execute(self, context: dict) -> dict: return {"val": 1}
    def compensate(self, context: dict) -> None: pass

def test_executor_retry_logic():
    executor = DAGExecutor(tenant_id=uuid4(), max_retries=2)
    
    class FailingStep:
        def __init__(self): self.calls = 0
        def execute(self, context: dict) -> dict:
            self.calls += 1
            raise ValueError("Fail")
        def compensate(self, context: dict) -> None: pass

    step = FailingStep()
    with pytest.raises(StepExecutionError):
        executor.run_step_with_retry(step, {})
    
    assert step.calls == 2
```

# Validation Criteria

- **Tenant Isolation**: A query for `Workflow` objects using `tenant_id=A` must return zero results if the request context is `tenant_id=B`.
- **Idempotency**: Calling `process_webhook` twice with the same `x-idempotency-key` must result in the second call raising `IdempotencyConflictError`.
- **DAG Integrity**: A workflow with a circular dependency must fail validation before execution begins.
- **Compensating Actions**: If step 3 of 5 fails, `compensate()` must be called exactly once for step 2 and step 1 in reverse order.
- **Signature Security**: A webhook payload modified by a single bit must fail `verify_signature`.

# Error Handling And Edge Cases

## Named Exceptions
- `TenantAccessDeniedError`: Raised when `tenant_id` in JWT does not match resource `tenant_id`.
- `IdempotencyConflictError`: Raised when `x-idempotency-key` exists in Redis.
- `StepExecutionError`: Raised when a DAG node exceeds `max_retries`.
- `DatabaseConnectionError`: Raised when PostgreSQL is unreachable.

## Edge Case Behavior
- **Empty Input**: 
    - Webhook with empty body: Return `400 Bad Request`.
    - Workflow with zero steps: Return empty success context.
- **None/Null Input**:
    - `tenant_id=None`: Raise `TenantAccessDeniedError`.
    - `context=None`: Initialize with empty dictionary `{}`.
- **Numeric Boundaries**:
    - `max_retries=0`: Execute step exactly once; fail immediately on error.
    - `max_retries < 0`: Raise `ValueError` during initialization.
    - `rate_limit=0`: Deny all requests immediately.
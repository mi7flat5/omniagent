# Summary

This specification defines the implementation of a production-grade, multi-tenant workflow automation platform. The system provides a DAG-based workflow engine, JWT-based RBAC authentication, and organization-level tenant isolation. It utilizes FastAPI for the REST interface, PostgreSQL for relational persistence, Redis for task queuing and rate limiting, and OpenTelemetry for observability. The platform ensures reliability through an outbox/inbox pattern and provides idempotent webhook ingestion via HMAC signature verification.

# Requirements

1.  **Multi-Tenancy**: Every database record and API request must be scoped to an `organization_id`.
2.  **Authentication**: Implement JWT-based authentication with Role-Based Access Control (RBAC) containing `admin`, `operator`, and `viewer` roles.
3.  **Workflow Engine**: Implement a Directed Acyclic Graph (DAG) engine where nodes represent tasks and edges represent dependencies.
4.  **Reliability**:
    *   Implement a retry mechanism with exponential backoff for failed steps.
    *   Implement compensating actions (rollbacks) for failed DAG executions.
    *   Use an Outbox pattern to ensure database updates and message queue emissions are atomic.
5.  **Webhook Ingestion**:
    *   Verify incoming webhooks using HMAC-SHA256 signatures.
    *   Enforce idempotency using a unique `idempotency_key` provided in headers.
6.  **Observability**:
    *   Emit OpenTelemetry traces for all workflow steps.
    *   Expose Prometheus metrics for task latency, failure rates, and queue depths.
7.  **Persistence**: Use PostgreSQL for all relational data and Redis for distributed locking and rate limiting.

# File Structure

```text
src/
├── auth/
│   ├── __init__.py
│   ├── dependencies.py
│   └── security.py
├── core/
│   ├── __init__.py
│   ├── config.py
│   └── exceptions.py
├── engine/
│   ├── __init__.py
│   ├── dag.py
│   ├── executor.py
│   └── models.py
├── webhooks/
│   ├── __init__.py
│   ├── handler.py
│   └── verifier.py
└── main.py
tests/
├── __init__.py
├── test_auth.py
├── test_engine.py
└── test_webhooks.py
```

# Implementation Details

## Core Exceptions

```python
from core.exceptions import WorkflowError, AuthenticationError, TenantIsolationError

class WorkflowError(Exception):
    """Base exception for all workflow-related failures."""
    pass

class DependencyError(WorkflowError):
    """Raised when a DAG dependency cannot be satisfied."""
    pass

class TenantIsolationError(Exception):
    """Raised when a user attempts to access data outside their organization."""
    pass
```

## Workflow Engine Implementation

The engine processes nodes based on their dependency state.

```python
from typing import Dict, List, Set, Any
from engine.models import TaskNode, TaskStatus
from engine.dag import DAG
from core.exceptions import DependencyError

class WorkflowExecutor:
    def __init__(self, dag: DAG, tenant_id: str) -> None:
        self.dag: DAG = dag
        self.tenant_id: str = tenant_id
        self.completed_nodes: Set[str] = set()

    def execute_step(self, node_id: str) -> TaskStatus:
        """
        Executes a single node in the DAG if dependencies are met.
        
        Args:
            node_id: The unique identifier of the task node.
            
        Returns:
            The resulting TaskStatus of the execution.
            
        Raises:
            DependencyError: If the node's dependencies are not in completed_nodes.
        """
        node: TaskNode = self.dag.get_node(node_id)
        
        for dep_id in node.dependencies:
            if dep_id not in self.completed_nodes:
                raise DependencyError(f"Dependency {dep_id} not met for {node_id}")
        
        # Logic for task execution goes here
        return TaskStatus.SUCCESS

    def run_all(self) -> Dict[str, TaskStatus]:
        """
        Iterates through the DAG and executes all available nodes.
        
        Returns:
            A mapping of node_id to TaskStatus.
        """
        results: Dict[str, TaskStatus] = {}
        # Implementation of topological sort and execution
        return results
```

## Webhook Verification

```python
import hmac
import hashlib
from typing import Dict
from webhooks.verifier import SignatureError

class WebhookVerifier:
    def __init__(self, secret_key: str) -> None:
        self.secret_key: str = secret_key

    def verify_signature(self, payload: bytes, signature: str) -> bool:
        """
        Verifies the HMAC-SHA256 signature of a webhook payload.
        
        Args:
            payload: The raw request body bytes.
            signature: The signature string from the X-Hub-Signature header.
            
        Returns:
            True if the signature is valid.
            
        Raises:
            SignatureError: If the signature does not match.
        """
        expected_signature: str = hmac.new(
            self.secret_key.encode(),
            payload,
            hashlib.sha256
        ).hexdigest()
        
        if not hmac.compare_digest(expected_signature, signature):
            raise SignatureError("Invalid HMAC signature")
            
        return True
```

# Testing Strategy

## Unit Tests

*   `tests/test_engine.py`:
    *   Test `WorkflowExecutor.execute_step` with satisfied dependencies.
    *   Test `WorkflowExecutor.execute_step` with unsatisfied dependencies to verify `DependencyError`.
    *   Test `WorkflowExecutor.run_all` with a cyclic graph to verify detection.
*   `tests/test_webhooks.py`:
    *   Test `WebhookVerifier.verify_signature` with valid HMAC signatures.
    *   Test `WebhookVerifier.verify_signature` with invalid signatures to verify `SignatureError`.

## Integration Tests

*   `tests/test_auth.py`:
    *   Call `auth.security.create_access_token` and use the resulting token in a FastAPI `TestClient` request to `/api/protected` to verify `200 OK`.
    *   Call `/api/protected` with an expired token to verify `401 Unauthorized`.
*   `tests/test_webhooks.py`:
    *   Call `webhooks.handler.ingest_webhook` with a valid `idempotency_key` twice to verify the second call returns the cached response without re-processing.

# Validation Criteria

1.  **Dependency Validation**: `WorkflowExecutor.execute_step("node_b")` must raise `DependencyError` if `node_a` (its dependency) is not in `completed_nodes`.
2.  **Signature Validation**: `WebhookVerifier.verify_signature(b"data", "wrong_sig")` must raise `SignatureError`.
3.  **Tenant Isolation**: A request to `/api/projects/{id}` with a valid JWT for `org_a` must return `403 Forbidden` if `{id}` belongs to `org_b`.
4.  **Idempotency**: `webhooks.handler.ingest_webhook(payload, key="unique_123")` must result in exactly one database write for the same `key`.

# Error Handling And Edge Cases

## Exceptions

*   `AuthenticationError`: Raised when JWT is missing, expired, or malformed.
*   `TenantIsolationError`: Raised when `request.org_id != resource.org_id`.
*   `SignatureError`: Raised when HMAC verification fails.
*   `DependencyError`: Raised when DAG constraints are violated.
*   `IdempotencyError`: Raised if an idempotency key is reused with a different payload.

## Edge Cases

*   **Empty Input**:
    *   `WorkflowExecutor.run_all()` on an empty DAG returns an empty dictionary `{}`.
    *   `WebhookVerifier.verify_signature(b"", signature)` returns `False` or raises `SignatureError` if the signature is invalid for an empty body.
*   **None/Null Input**:
    *   Passing `None` to `verify_signature` raises `TypeError`.
    *   Passing `None` to `execute_step` raises `ValueError`.
*   **Numeric Boundaries**:
    *   Retry counts of `0` result in zero retries (immediate failure).
    *   Negative retry counts raise `ValueError`.
    *   Rate limit thresholds of `0` result in immediate `429 Too Many Requests`.
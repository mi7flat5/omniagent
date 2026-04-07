# Summary

This specification defines the implementation of a production-grade, multi-tenant workflow automation platform. The system provides a DAG-based workflow engine with robust reliability patterns, including retries, compensating actions, and an outbox/inbox pattern for distributed consistency. It features strict tenant isolation at the organization level, JWT-based RBAC, and high-performance ingestion via HMAC-verified webhooks. The platform utilizes FastAPI for the REST interface, PostgreSQL for relational persistence, Redis for queuing and rate limiting, and OpenTelemetry for observability.

# Requirements

## Authentication and Authorization
- Implement JWT-based authentication.
- Enforce Role-Based Access Control (RBAC) with roles: `admin`, `operator`, and `user`.
- Implement organization-level tenant isolation; every database query must include an `organization_id` filter.

## Workflow Engine
- Support Directed Acyclic Graph (DAG) workflow definitions.
- Implement step dependencies where a step executes only after its parent nodes succeed.
- Provide retry logic with exponential backoff for failed steps.
- Implement compensating actions (undo logic) for steps that fail after maximum retries are exhausted.

## Webhook Ingestion
- Provide an endpoint for external webhook ingestion.
- Enforce HMAC signature verification using a shared secret per tenant.
- Implement idempotency using `idempotency_key` headers to prevent duplicate processing.

## Reliability and Persistence
- Use the Outbox Pattern: write events to a `persistent_outbox` table within the same transaction as the business logic.
- Use the Inbox Pattern: track processed `event_ids` to ensure exactly-once processing of incoming events.
- Maintain an immutable audit log of all system actions and workflow transitions.

## Observability and Scaling
- Integrate OpenTelemetry for distributed tracing across the API and worker processes.
- Export Prometheus metrics for workflow success rates, latency, and queue depths.
- Implement Redis-backed rate limiting per tenant and per user.

# File Structure

```text
.
├── src/
│   ├── api/
│   │   ├── auth.py
│   │   ├── webhooks.py
│   │   └── workflows.py
│   ├── core/
│   │   ├── config.py
│   │   ├── security.py
│   │   └── exceptions.py
│   ├── engine/
│   │   ├── dag.py
│   │   ├── executor.py
│   │   └── scheduler.py
│   ├── models/
│   │   ├── database.py
│   │   ├── tenant.py
│   │   └── workflow.py
│   ├── services/
│   │   ├── outbox.py
│   │   └── webhook_verifier.py
│   └── main.py
├── tests/
│   ├── unit/
│   │   ├── test_auth.py
│   │   └── test_dag.py
│   ├── integration/
│   │   ├── test_webhook_flow.py
│   │   └── test_workflow_execution.py
│   └── conftest.py
├── migrations/
├── docker-compose.yml
└── pyproject.toml
```

# Implementation Details

## Error Definitions

```python
from typing import Any

class OmniAgentError(Exception):
    """Base exception for the platform."""
    pass

class AuthenticationError(OmniAgentError):
    """Raised when JWT validation fails."""
    pass

class TenantIsolationError(OmniAgentError):
    """Raised when a user attempts to access data outside their organization."""
    pass

class WorkflowExecutionError(OmniAgentError):
    """Raised when a DAG step fails and cannot be recovered."""
    pass

class InvalidSignatureError(OmniAgentError):
    """Raised when HMAC verification fails for webhooks."""
    pass

class IdempotencyConflictError(OmniAgentError):
    """Raised when a duplicate idempotency key is detected."""
    pass
```

## Webhook Verification Logic

```python
import hmac
import hashlib
from typing import Dict
from src.core.exceptions import InvalidSignatureError

class WebhookVerifier:
    def __init__(self, secret: str) -> None:
        self.secret: str = secret

    def verify_signature(self, payload: bytes, signature: str) -> bool:
        """
        Verifies the HMAC SHA256 signature of the payload.
        
        Args:
            payload: The raw request body bytes.
            signature: The hex-encoded signature from the header.
            
        Returns:
            True if valid.
            
        Raises:
            InvalidSignatureError: If the signature does not match.
        """
        expected_signature: str = hmac.new(
            self.secret.encode("utf-8"),
            payload,
            hashlib.sha256
        ).hexdigest()

        if not hmac.compare_digest(expected_signature, signature):
            raise InvalidSignatureError("Signature mismatch")
        
        return True
```

## Workflow Execution Engine

```python
from typing import List, Dict, Any, Optional
from src.models.workflow import WorkflowStep, WorkflowStatus
from src.core.exceptions import WorkflowExecutionError

class WorkflowExecutor:
    def __init__(self, tenant_id: str) -> None:
        self.tenant_id: str = tenant_id

    def execute_step(self, step: WorkflowStep, context: Dict[str, Any]) -> Dict[str, Any]:
        """
        Executes a single step in the workflow.
        
        Args:
            step: The step definition to execute.
            context: The shared state for the workflow run.
            
        Returns:
            The output of the step.
            
        Raises:
            WorkflowExecutionError: If the step fails after all retries.
        """
        attempts: int = 0
        max_retries: int = step.retry_policy.max_attempts

        while attempts <= max_retries:
            try:
                # Logic for executing the actual task
                result: Dict[str, Any] = self._run_task(step, context)
                return result
            except Exception as e:
                attempts += 1
                if attempts > max_retries:
                    self._trigger_compensation(step, context)
                    raise WorkflowExecutionError(f"Step {step.id} failed after {attempts} attempts") from e
                self._apply_backoff(attempts)
        
        return {}

    def _run_task(self, step: WorkflowStep, context: Dict[str, Any]) -> Dict[str, Any]:
        # Implementation of task execution
        return {"status": "success"}

    def _trigger_compensation(self, step: WorkflowStep, context: Dict[str, Any]) -> None:
        # Logic to execute the compensating action defined in the step
        pass

    def _apply_backoff(self, attempt: int) -> None:
        # Exponential backoff logic
        pass
```

# Testing Strategy

## Unit Testing
- Test `WebhookVerifier.verify_signature` with valid, invalid, and empty signatures.
- Test `WorkflowExecutor.execute_step` with successful execution, retryable failures, and terminal failures.
- Test RBAC logic by attempting to access `organization_id` B with a token for `organization_id` A.

## Integration Testing
- **Webhook Flow**:
  ```python
  def test_webhook_ingestion_flow(client: TestClient, verifier: WebhookVerifier):
      payload = b'{"event": "test"}'
      signature = "correct_signature_here"
      response = client.post("/api/webhooks/ingest", content=payload, headers={"X-Signature": signature})
      assert response.status_code == 202
  ```
- **Workflow DAG**:
  ```python
  def test_dag_execution_order(db_session: Session):
      # Setup a DAG: Step A -> Step B
      # Execute workflow
      # Verify Step B only runs after Step A completes
      pass
  ```

# Validation Criteria

- **Tenant Isolation**: A query for `Workflow` where `organization_id != user.organization_id` must return 0 results.
- **Idempotency**: Sending two requests with the same `X-Idempotency-Key` must result in the second request returning the cached response of the first, with no duplicate database entries.
- **Signature Integrity**: Any webhook request with a missing or incorrect `X-Signature` must return `401 Unauthorized`.
- **Empty Input**:
    - `execute_step` with an empty context must return an error if the step requires context.
    - `verify_signature` with an empty payload must return `False` or raise `InvalidSignatureError`.
- **Numeric Boundaries**:
    - Retry counts must be non-negative integers.
    - Rate limit values must be greater than zero.

# Error Handling And Edge Cases

| Scenario | Expected Behavior |
| :--- | :--- |
| **Empty Input** | API returns `400 Bad Request` with a validation error message. |
| **None/Null Input** | All service methods must raise `ValueError` or `TypeError` immediately. |
| **Zero/Negative Retries** | The engine must treat 0 retries as "execute once, fail immediately on error". |
| **Invalid JWT** | Return `401 Unauthorized`. |
| **Insufficient Permissions** | Return `403 Forbidden`. |
| **Database Connection Loss** | The Outbox pattern must ensure that once the DB is restored, the background worker picks up pending events. |
| **Webhook Signature Mismatch** | Return `401 Unauthorized` and log a security warning. |
| **Duplicate Idempotency Key** | Return `409 Conflict` or the original successful response. |
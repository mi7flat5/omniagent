# Summary

A multi-tenant webhook relay service built with FastAPI. The service receives incoming webhooks, verifies their authenticity using HMAC signatures, ensures idempotency via unique keys, and dispatches them to configured destination URLs. It features a robust reliability layer including a retry queue with exponential backoff, a dead-letter queue (DLQ) for failed deliveries, and an administrative interface for replaying failed webhooks. Persistence is handled via PostgreSQL.

# Requirements

- **Multi-tenancy**: Isolate webhook configurations and delivery logs by `tenant_id`.
- **HMAC Verification**: Validate incoming payloads using a tenant-specific secret and a configurable header key.
- **Idempotency**: Prevent duplicate processing of the same webhook event using an `Idempotency-Key` header.
- **Reliability**:
    - Implement exponential backoff for retries (e.g., $2^n$ seconds).
    - Move events to a Dead-Letter Queue (DLQ) after a maximum number of retry attempts.
- **Persistence**: Store tenant configurations, webhook event logs, and retry states in PostgreSQL.
- **Admin API**: Provide endpoints to list DLQ events and trigger manual replays.
- **Concurrency**: Use asynchronous processing for webhook dispatching to prevent blocking the ingestion endpoint.

# File Structure

```text
webhook_relay/
├── src/
│   ├── __init__.py
│   ├── main.py
│   ├── api/
│   │   ├── __init__.py
│   │   ├── auth.py
│   │   ├── ingest.py
│   │   └── admin.py
│   ├── core/
│   │   ├── __init__.py
│   │   ├── config.py
│   │   ├── exceptions.py
│   │   └── security.py
│   ├── models/
│   │   ├── __init__.py
│   │   ├── database.py
│   │   └── schemas.py
│   ├── services/
│   │   ├── __init__.py
│   │   ├── dispatcher.py
│   │   └── retry_manager.py
│   └── worker/
│       ├── __init__.py
│       └── task_processor.py
├── tests/
│   ├── __init__.py
│   ├── test_ingestion.py
│   ├── test_security.py
│   └── test_retry_logic.py
├── alembic/
├── pyproject.toml
└── README.md
```

# Implementation Details

### Core Exceptions
```python
from typing import Any

class WebhookError(Exception):
    """Base exception for the service."""
    pass

class InvalidSignatureError(WebhookError):
    """Raised when HMAC verification fails."""
    pass

class TenantNotFoundError(WebhookError):
    """Raised when the provided tenant_id does not exist."""
    pass

class IdempotencyConflictError(WebhookError):
    """Raised when a duplicate idempotency key is detected."""
    pass

class DispatchError(WebhookError):
    """Raised when the destination URL returns a non-2xx status."""
    pass
```

### Security and Verification
```python
import hmac
import hashlib
from typing import Dict
from webhook_relay.core.exceptions import InvalidSignatureError

def verify_hmac_signature(
    payload: bytes, 
    signature: str, 
    secret: str, 
    header_name: str = "X-Hub-Signature-256"
) -> bool:
    """
    Verifies that the payload matches the provided HMAC signature.
    
    Args:
        payload: The raw request body.
        signature: The hex-encoded signature from the header.
        secret: The tenant-specific secret key.
        header_name: The name of the header containing the signature.
        
    Returns:
        bool: True if valid.
        
    Raises:
        InvalidSignatureError: If the signature does not match.
    """
    expected_signature = hmac.new(
        secret.encode("utf-8"),
        payload,
        hashlib.sha256
    ).hexdigest()
    
    if not hmac.compare_digest(expected_signature, signature):
        raise InvalidSignatureError("Signature mismatch")
    
    return True
```

### Dispatcher Service
```python
import httpx
from typing import Dict, Any
from webhook_relay.core.exceptions import DispatchError

class WebhookDispatcher:
    def __init__(self, client: httpx.AsyncClient):
        self.client = client

    async def dispatch(
        self, 
        url: str, 
        payload: Dict[str, Any], 
        headers: Dict[str, str]
    ) -> httpx.Response:
        """
        Sends the webhook payload to the destination URL.
        
        Args:
            url: The destination endpoint.
            payload: The JSON payload to send.
            headers: Additional headers to include.
            
        Returns:
            httpx.Response: The response from the destination.
            
        Raises:
            DispatchError: If the response status code is not 2xx.
        """
        try:
            response = await self.client.post(
                url, 
                json=payload, 
                headers=headers, 
                timeout=10.0
            )
            if not (200 <= response.status_code < 300):
                raise DispatchError(f"Destination returned {response.status_code}")
            return response
        except httpx.RequestError as exc:
            raise DispatchError(f"Network error: {str(exc)}")
```

# Testing Strategy

- **Unit Tests**: Test `verify_hmac_signature` with valid, invalid, and empty signatures.
- **Integration Tests**: Use `httpx.ASGITransport` to test the full ingestion flow from `POST /ingest/{tenant_id}` to database persistence.
- **Retry Logic Tests**: Mock the `WebhookDispatcher` to fail $N$ times and verify the `retry_manager` calculates the correct exponential backoff interval.
- **Concurrency Tests**: Simulate multiple simultaneous requests with the same `Idempotency-Key` to ensure only one is processed.

**Example Test Case:**
```python
import pytest
from webhook_relay.core.security import verify_hmac_signature
from webhook_relay.core.exceptions import InvalidSignatureError

def test_verify_hmac_signature_success() -> None:
    payload = b'{"event": "test"}'
    secret = "super-secret"
    # Pre-calculated signature for b'{"event": "test"}' with secret "super-secret"
    valid_sig = "7696666666666666666666666666666666666666666666666666666666666666" # Placeholder
    # In real test, use actual hmac.new().hexdigest()
    assert verify_hmac_signature(payload, valid_sig, secret) is True

def test_verify_hmac_signature_failure() -> None:
    payload = b'{"event": "test"}'
    secret = "super-secret"
    invalid_sig = "wrong-signature"
    with pytest.raises(InvalidSignatureError):
        verify_hmac_signature(payload, invalid_sig, secret)
```

# Validation Criteria

- **Signature Validation**: Calling `POST /ingest/{tenant_id}` with an incorrect `X-Signature` header must return `401 Unauthorized`.
- **Idempotency**: Sending two requests with `Idempotency-Key: key_123` within 1 second must result in the first request returning `202 Accepted` and the second returning `409 Conflict`.
- **Retry Exhaustion**: An event that fails 5 consecutive times must have its status updated to `DLQ` in the database.
- **Admin Replay**: Calling `POST /admin/replay/{event_id}` must change the event status from `DLQ` back to `PENDING` and trigger a new dispatch attempt.

# Error Handling And Edge Cases

- **Empty Payload**: If the request body is empty, return `400 Bad Request`.
- **None/Null Tenant ID**: If `tenant_id` is not provided in the path, return `404 Not Found`.
- **Invalid JSON**: If the payload is not valid JSON, return `400 Bad Request`.
- **Zero/Negative Retry Count**: If a configuration specifies 0 retries, the event must move to `DLQ` immediately upon the first failure.
- **Database Connection Failure**: If PostgreSQL is unreachable, return `503 Service Unavailable`.
- **Malformed HMAC Header**: If the signature header is missing or not hex-encoded, return `401 Unauthorized`.
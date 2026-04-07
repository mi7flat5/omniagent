# Summary
The Webhook Relay Service is a high-reliability FastAPI application designed to ingest webhooks from external providers, verify their authenticity via HMAC signatures, and ensure guaranteed delivery to downstream target URLs. The service implements idempotency via unique keys, manages retries using an exponential backoff strategy, and provides a Dead Letter Queue (DLQ) for failed deliveries. All event states and retry attempts are persisted in a PostgreSQL database.

# Requirements
1.  **Ingestion**: The service provides a `POST /webhook/{provider}` endpoint.
2.  **Authentication**: Every request to the webhook endpoint must include an `X-Signature` header. The service verifies this signature using a provider-specific secret stored in the database.
3.  **Idempotency**: The service requires an `X-Idempotency-Key` header. If a key is reused for the same provider, the service returns a `409 Conflict` and does not process the payload.
4.  **Persistence**: All incoming payloads, provider configurations, and delivery attempts are stored in PostgreSQL.
5.  **Retry Mechanism**: Failed deliveries trigger an asynchronous retry loop. The backoff follows the formula: $delay = 2^{attempt} \text{ seconds}$. The maximum number of retries is exactly 5.
6.  **Dead Letter Queue**: After the 5th failed attempt, the event status is set to `FAILED` and moved to the DLQ.
7.  **Admin Interface**:
    *   `POST /admin/replay/{event_id}`: Re-triggers the delivery of a specific event.
    *   `GET /admin/dlq`: Returns a list of all events with status `FAILED`.

# File Structure
main.py
database.py
models.py
schemas.py
security.py
worker.py
exceptions.py
tests/test_api.py
tests/test_worker.py

# Implementation Details

The service uses SQLAlchemy for PostgreSQL interaction and `httpx` for downstream delivery.

### Security Implementation
The `security.py` module handles HMAC verification.

```python
import hmac
import hashlib
from exceptions import InvalidSignatureError

def verify_hmac_signature(payload: bytes, signature: str, secret: str) -> bool:
    """
    Verifies that the provided signature matches the HMAC-SHA256 hash of the payload.
    """
    if not secret:
        raise ValueError("Secret must be a non-empty string")
    
    expected_signature: str = hmac.new(
        secret.encode("utf-8"), 
        payload, 
        hashlib.sha256
    ).hexdigest()
    
    if not hmac.compare_digest(expected_signature, signature):
        raise InvalidSignatureError(f"Signature mismatch: expected {expected_signature}, got {signature}")
    
    return True
```

### Worker Implementation
The `worker.py` module manages the asynchronous retry logic.

```python
import asyncio
import httpx
from datetime import datetime, timedelta
from models import Event, EventStatus
from database import SessionLocal
from exceptions import TargetDeliveryError

async def execute_retry_logic(event_id: str, attempt: int) -> None:
    """
    Executes a single delivery attempt with exponential backoff.
    """
    db = SessionLocal()
    try:
        event: Event = db.query(Event).filter(Event.id == event_id).first()
        if event is None:
            return

        # Calculate delay: 2^attempt
        delay_seconds: int = 2 ** attempt
        await asyncio.sleep(delay_seconds)

        async with httpx.AsyncClient() as client:
            response = await client.post(event.target_url, content=event.payload, headers=event.headers)
            
            if response.status_code >= 200 and response.status_code < 300:
                event.status = EventStatus.DELIVERED
            else:
                raise TargetDeliveryError(f"Status code: {response.status_code}")
                
    except Exception:
        if attempt < 5:
            # Schedule next attempt
            asyncio.create_task(execute_retry_logic(event_id, attempt + 1))
        else:
            event.status = EventStatus.FAILED
    finally:
        db.commit()
        db.close()
```

# Testing Strategy
The testing suite uses `pytest` and `httpx.ASGITransport` for integration testing.

1.  **Unit Tests**:
    *   `test_verify_hmac_signature_success`: Call `verify_hmac_signature(b'data', 'correct_hash', 'secret')`.
    *   `test_verify_hmac_signature_failure`: Call `verify_hmac_signature(b'data', 'wrong_hash', 'secret')` and assert `InvalidSignatureError` is raised.
2.  **Integration Tests**:
    *   `test_webhook_ingestion_flow`: Send a valid `POST /webhook/stripe` request and verify the database contains a new `Event` record with status `PENDING`.
    *   `test_idempotency_enforcement`: Send two identical requests with the same `X-Idempotency-Key` and verify the second request returns `409`.
    *   `test_retry_exhaustion`: Mock `httpx.AsyncClient` to return `500 Internal Server Error` and verify the event status transitions to `FAILED` after exactly 5 attempts.

# Validation Criteria
1.  **Signature Validation**: A request with an invalid `X-Signature` must return `401 Unauthorized`.
2.  **Idempotency**: A request with a duplicate `X-Idempotency-Key` for the same provider must return `409 Conflict`.
3.  **Retry Accuracy**: An event failing 5 times must have its `retry_count` column equal to `5` and its `status` equal to `FAILED`.
4.  **Admin Replay**: Calling `POST /admin/replay/{event_id}` must reset the `retry_count` to `0` and change the status to `PENDING`.
5.  **DLQ Retrieval**: `GET /admin/dlq` must return exactly the number of records where `status == 'FAILED'`.

# Error Handling And Edge Cases

### Named Exceptions
*   `InvalidSignatureError`: Raised when HMAC verification fails.
*   `IdempotencyConflictError`: Raised when a duplicate idempotency key is detected.
*   `TargetDeliveryError`: Raised when the downstream HTTP request returns a non-2xx status code.
*   `ProviderNotFoundError`: Raised when the requested provider does not exist in the database.

### Edge Case Behavior
*   **Empty Payload**: If the request body is empty, the service returns `400 Bad Request`.
*   **None/Null Input**: If `X-Signature` or `X-Idempotency-Key` headers are missing, the service returns `400 Bad Request`.
*   **Zero/Negative Retry Count**: If the database contains a negative `retry_count`, the worker treats it as `0`.
*   **Invalid Provider**: If the `{provider}` path parameter does not match a registered provider in the database, the service returns `404 Not Found`.
*   **Database Connection Failure**: If PostgreSQL is unreachable, the service returns `503 Service Unavailable`.
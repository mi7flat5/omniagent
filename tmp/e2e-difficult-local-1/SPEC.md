# Summary
This specification defines a multi-tenant webhook relay service built with FastAPI and PostgreSQL. The service receives incoming webhooks, verifies their authenticity via HMAC-SHA256 signatures, ensures idempotency using unique keys, and manages delivery to downstream targets with an exponential backoff retry mechanism and a dead-letter queue (DLQ). An administrative CLI provides functionality to replay events from the DLQ.

# Requirements
1. **Multi-tenancy**: Every webhook request must include a `tenant_id` used to look up the unique HMAC secret and target URL.
2. **Signature Verification**: The service must verify the `X-Signature` header using HMAC-SHA256 and the tenant's secret.
3. **Idempotency**: The service must enforce idempotency using the `X-Idempotency-Key` header. If a key exists for a specific tenant, the service returns the cached response.
4. **Persistence**: All incoming events, delivery attempts, and statuses must be stored in a PostgreSQL database.
5. **Retry Mechanism**:
    - The service must attempt delivery up to 5 times.
    - The delay between retries follows the formula: $delay = 2^{attempt} \times 10$ seconds.
    - After 5 failed attempts, the event status must be set to `DEAD_LETTER`.
6. **Admin CLI**: A command-line interface must allow an administrator to trigger a replay of events with status `DEAD_LETTER`.

# File Structure
src/
  app/
    __init__.py
    main.py
    api/
      __init__.py
      v1/
        __init__.py
        endpoints.py
    core/
      __init__.py
      security.py
      config.py
    models/
      __init__.py
      webhook.py
    services/
      __init__.py
      relay.py
    exceptions/
      __init__.py
      custom.py
    db/
      __init__.py
      session.py
  cli/
    __init__.py
    admin.py
tests/
  __init__.py
  test_api.py
  test_services.py
  test_security.py

# Implementation Details

## Database Schema
The `webhook_events` table contains:
- `id`: UUID (Primary Key)
- `tenant_id`: UUID (Indexed)
- `idempotency_key`: String (Indexed)
- `payload`: JSONB
- `status`: Enum (RECEIVED, PROCESSING, DELIVERED, DEAD_LETTER)
- `retry_count`: Integer (Default 0)
- `next_retry_at`: Timestamp (Nullable)
- `created_at`: Timestamp

## Security Implementation
The signature verification uses `hmac.compare_digest` to prevent timing attacks.

```python
import hmac
import hashlib
from src.app.exceptions.custom import InvalidSignatureError

def verify_hmac_signature(payload: bytes, signature: str, secret: str) -> bool:
    """
    Verifies that the provided signature matches the HMAC-SHA256 hash of the payload.
    """
    if not signature:
        raise InvalidSignatureError("Signature header is missing.")
    
    expected_signature: str = hmac.new(
        secret.encode("utf-8"),
        payload,
        hashlib.sha256
    ).hexdigest()
    
    if not hmac.compare_digest(expected_signature, signature):
        raise InvalidSignatureError(f"Signature mismatch.")
        
    return True
```

## Relay Service Implementation
The relay service manages the lifecycle of a webhook event.

```python
from typing import Dict, Any
from src.app.models.webhook import WebhookEvent, EventStatus
from src.app.core.security import verify_hmac_signature
from src.app.exceptions.custom import IdempotencyConflictError, TenantNotFoundError, InvalidSignatureError

def process_webhook_event(
    tenant_id: str,
    payload: Dict[str, Any],
    signature: str,
    idempotency_key: str,
    secret: str
) -> WebhookEvent:
    """
    Validates, persists, and initiates the delivery of a webhook event.
    """
    # 1. Verify Tenant (Logic omitted for brevity)
    # 2. Check Idempotency (Logic omitted for brevity)
    # 3. Verify Signature
    payload_bytes: bytes = str(payload).encode("utf-8")
    verify_hmac_signature(payload_bytes, signature, secret)
    
    # 4. Create Event
    event: WebhookEvent = WebhookEvent(
        tenant_id=tenant_id,
        payload=payload,
        idempotency_key=idempotency_key,
        status=EventStatus.RECEIVED
    )
    return event
```

# Testing Strategy
1. **Unit Tests**: Test `verify_hmac_signature` with valid and invalid signatures.
2. **Integration Tests**: Test the full API flow from POST request to database persistence.
3. **Retry Logic Tests**: Mock the downstream target to return 500 errors and verify the `retry_count` increments and status changes to `DEAD_LETTER`.
4. **CLI Tests**: Execute the admin CLI command and verify database status updates.

**Usage Examples**:
- `test_verify_hmac_signature_success()`: Calls `verify_hmac_signature(b"data", "valid_hash", "secret")` and asserts `True`.
- `test_ingest_webhook_api()`: Calls `client.post("/v1/webhooks", json={"data": "val"}, headers={"X-Idempotency-Key": "key1"})` and asserts `201 Created`.
- `test_replay_cli()`: Calls `subprocess.run(["python", "-m", "src.cli.admin", "replay", "tenant_uuid"])` and asserts the event status in the database is `PROCESSING`.

# Validation Criteria
1. A request with an invalid `X-Signature` returns `403 Forbidden`.
2. A request with a duplicate `X-Idempotency-Key` for the same `tenant_id` returns `409 Conflict`.
3. A request with a non-existent `tenant_id` returns `404 Not Found`.
4. An event that fails 5 delivery attempts has `status = 'DEAD_LETTER'` in the database.
5. The `next_retry_at` timestamp for the second retry attempt is exactly 20 seconds after the first attempt.

# Error Handling And Edge Cases
## Named Exceptions
- `InvalidSignatureError`: Raised when HMAC verification fails.
- `IdempotencyConflictError`: Raised when the `idempotency_key` is already present for the tenant.
- `TenantNotFoundError`: Raised when the `tenant_id` does not exist in the database.
- `PayloadValidationError`: Raised when input data is malformed.

## Edge Cases
- **Empty Payload**: If `payload` is `{}` or `None`, the service raises `PayloadValidationError`.
- **Empty Signature**: If `signature` is `""`, the service raises `InvalidSignatureError`.
- **Empty Idempotency Key**: If `idempotency_key` is `""`, the service raises `PayloadValidationError`.
- **Zero/Negative Retry Count**: The `retry_count` must be an integer $\ge 0$. The system initializes it at 0.
- **Missing Tenant ID**: If `tenant_id` is not provided in the request, the service returns `400 Bad Request`.
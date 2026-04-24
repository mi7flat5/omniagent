# Confctl Source-of-Truth Audit

Date: 2026-04-08

## Scope

- Target repository audited: `/home/mi7fl/projects/confctl`
- Output intentionally stored outside the target repo
- Evidence sources: current checked-out source, current unit tests, and a fresh `pytest -q` baseline
- No code was modified in the target repository for this audit

## Baseline

- Command run: `pytest -q`
- Result: `42 failed, 72 passed in 1.19s`
- The current failures cluster around schema loading, store/type handling, CLI contract mismatches, env-loader normalization, and one low-severity package metadata issue

## Findings

### High: `load_schema()` cannot load the schema format the tests and builder expect

Evidence:

- `confctl/schema.py` iterates `schema_data.items()` directly instead of descending into a top-level `fields` mapping.
- `tests/test_schema.py` writes schema YAML shaped as:

```yaml
fields:
  app.name:
    type: string
```

- The fresh test baseline includes `ValueError: Missing required field 'type' for field 'fields'`, which matches the current implementation path exactly.
- `validate_config()` in the same module validates only flat dotted keys, while the schema tests pass nested config dicts such as `{"app": {"name": "MyApp"}}`.

Impact:

- A schema file in the format exercised by the tests fails before normal validation can begin.
- The module currently exposes two incompatible contracts at once: top-level `fields:` in tests versus direct root-field iteration in code, and nested config dicts in tests versus flat dotted-key validation in code.

### High: `ConfigStore` uses a schema-field model that contradicts `confctl.models`

Evidence:

- `confctl/models.py` defines `SchemaField.value_type`, not `SchemaField.type`.
- `confctl/store.py::_cast_value()` dereferences `field.type` and compares it against string literals such as `"int"` and `"bool"`.
- The fresh test baseline includes `AttributeError: 'dict' object has no attribute 'type'` from `confctl/store.py`, matching the current implementation.
- `tests/test_store.py` exercises both `SchemaField`-shaped data and dict-backed field definitions, which the current store code does not normalize or defend against.
- `confctl/models.py` declares `ConfigDiff.added`, `removed`, and `changed` as lists of `ConfigValue` or tuples, but `confctl/store.py::diff()` currently returns plain dicts.

Impact:

- Type casting, default propagation, and diff reporting are not trustworthy.
- Builder precedence failures are downstream of this mismatch because schema defaults and string-to-type casting both flow through the store.

### High: CLI parsing and command behavior diverge from the tested invocation contract

Evidence:

- `confctl/cli.py` defines `--schema`, `--config`, and `--set` only as global options parsed before subcommands.
- `tests/test_cli.py` expects forms such as `confctl get database.host --schema schema.yaml` to succeed.
- `confctl/cli.py` requires `diff file1 file2`, while the tests invoke `diff` with only one positional config path and expect command-specific handling.
- `validate` currently returns a generic failure when `--schema` is omitted, but the tests exercise more specific command-level behavior.

Impact:

- Even if the schema and store layers were fixed, the tested CLI surface would still fail in several places.
- This is a real contract mismatch between the parser shape and the exercised command UX, not just fallout from the schema bugs.

### Medium: Environment variable loading does not match the tested path-normalization rules

Evidence:

- `confctl/loader.py::load_env_vars()` accepts only variables beginning with `prefix + "_"`, which means `prefix=""` implicitly requires a leading underscore.
- `tests/test_loader.py` expects empty-prefix loading to accept plain names such as `A`.
- The loader preserves double underscores as literal `__`, while the tests explicitly expect `CONFCTL_A__B` to map to `a.b`.

Impact:

- Env-derived configuration does not currently land on the paths the tests and likely intended API expect.
- Builder env precedence behavior remains suspect until loader normalization is aligned with the tested contract.

### Low: Package docstring is not exposed as `confctl.__doc__`

Evidence:

- `confctl/__init__.py` places the package docstring after imports and `__all__`.
- `tests/test_confctl_init.py::test_package_docstring` expects `confctl.__doc__` to be populated and to contain `"confctl"`.

Impact:

- This is mechanically small, but it still produces a real package-level failure.

## Cross-Cutting Assessment

- The failures are not random. The suite points to four primary contract breaks:
  1. schema file shape and validation shape,
  2. store/schema model mismatch,
  3. CLI parser contract mismatch,
  4. env-loader path normalization mismatch.
- Builder failures are real, but most appear to be downstream of the schema/store/loader defects rather than a cleanly separate root cause.
- This audit does not claim independent security, performance, or production-readiness defects beyond the evidence above. The current evidence is dominated by broken runtime contracts and failing tests, not by demonstrated exploit paths.

## Recommended Remediation Order

1. Repair schema loading and config validation around the tested top-level `fields` format and nested-input shape.
2. Make `ConfigStore` consume the actual `SchemaField` model and emit `ConfigDiff` in the declared shape.
3. Bring CLI parsing, arity, and flag placement into alignment with the tested command contract.
4. Fix env-var normalization for empty-prefix and double-underscore cases.
5. Move the package docstring to the top of `confctl/__init__.py`.
6. Re-run `pytest -q` and only then classify any remaining failures as second-order defects.
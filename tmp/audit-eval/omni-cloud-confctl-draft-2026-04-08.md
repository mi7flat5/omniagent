# confctl Code Audit — Findings-First Report

## F1. ConfigStore._cast_value uses wrong attribute name (CRITICAL)
**File**: `confctl/store.py`, line 26  
**Bug**: `expected_type = field.type` — `SchemaField` has `value_type`, not `type`. This causes `AttributeError` at runtime whenever casting is attempted with a schema set.  
**Impact**: Any `set()` or `merge()` call on a `ConfigStore` with a schema crashes immediately.  
**Fix**: Change `field.type` → `field.value_type`.

Additionally, the string comparisons (`expected_type == "bool"`, `expected_type == "int"`, etc.) will never match because `value_type` is a `ValueType` enum, not a string. Must compare against `ValueType.BOOL`, `ValueType.INT`, etc.

## F2. ConfigStore.diff() returns wrong types in ConfigDiff (CRITICAL)
**File**: `confctl/store.py`, lines 244–248  
**Bug**: `diff()` constructs dicts for `added`/`removed`/`changed` but `ConfigDiff` expects `list[ConfigValue]`, `list[ConfigValue]`, and `list[tuple[ConfigValue, ConfigValue]]`. The method passes plain dicts of `{path: value}` instead of lists of `ConfigValue` objects.  
**Impact**: CLI `diff` command (cli.py:140–153) accesses `item.path` and `item.value` on diff entries, which will raise `AttributeError` at runtime since iterating a dict yields string keys, not `ConfigValue` objects.

## F3. load_schema does not unwrap `fields` key from schema YAML (HIGH)
**File**: `confctl/schema.py`, line 28  
**Bug**: `load_schema` iterates `schema_data.items()` directly, but the SPEC defines schema YAML with a top-level `fields` key. The tests also write schema YAML wrapped in a `fields` key. Without unwrapping, the function would treat `"fields"` as a field name and fail with "Missing required field 'type' for field 'fields'".  
**Impact**: Schema loading fails for any YAML file matching the spec format.  
**Fix**: Insert `schema_data = schema_data.get("fields", schema_data)` before the iteration loop if schema_data is a dict containing a 'fields' key.

## F4. validate_config receives nested dicts but expects flat dicts (HIGH)
**File**: `confctl/schema.py`, line 69  
**Bug**: `validate_config` checks `if field_name in config` where `field_name` is like `"app.name"`, but `ConfigStore.validate()` passes `store.to_dict()` which is a flat dict of `{dot.path: value}`. However, the schema tests pass nested dicts like `{"app": {"name": "MyApp"}}` directly to `validate_config`, which would fail since `"app.name" in {"app": {...}}` is `False`.  
**Impact**: The tests for `validate_config` with nested config dicts would fail. The production path works correctly because `to_dict()` returns flat dicts.  
**Note**: This is primarily a test bug, not an implementation bug for the main code path.

## F5. CLI `get` command treats falsy but non-None values as missing (MEDIUM)
**File**: `confctl/cli.py`, line 89  
**Bug**: `if value is not None:` correctly handles `0`, `False`, and `""`. However, if a key's value is explicitly `None`, the CLI reports "Path not found" because `ConfigStore.get()` returns `None` both for missing paths and for paths with value `None`.  
**Impact**: Cannot distinguish between a missing path and a path with value `None`.  
**Fix**: Check path existence separately.

## F6. CLI `diff` subcommand mismatch with SPEC (MEDIUM)
**File**: `confctl/cli.py`, lines 55–57  
**Bug**: The diff subparser uses positional args `file1` and `file2`, but the SPEC indicates a different CLI contract.

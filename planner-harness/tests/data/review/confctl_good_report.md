# Confctl Audit

- High: `confctl/schema.py::load_schema()` reads the top-level YAML mapping directly instead of descending into the `fields` mapping that `tests/test_schema.py` writes. The fresh `pytest -q` baseline was `42 failed, 72 passed in 1.19s`, and one of the direct failures is `Missing required field 'type' for field 'fields'`.
- High: `ConfigStore` in `confctl/store.py` is wired to a different schema-field model than `confctl/models.py`. `_cast_value()` dereferences `field.type`, but the declared model uses `value_type`, and the current baseline includes the matching `AttributeError` path.
- High: The CLI contract implemented in `confctl/cli.py` does not match the exercised contract in `tests/test_cli.py`. The parser only accepts global flags such as `--schema` before subcommands and requires `diff file1 file2`, while the tests expect post-subcommand `--schema` handling and single-argument `diff` behavior.
- Medium: `load_env_vars()` in `confctl/loader.py` does not match `tests/test_loader.py`. The current code rejects empty-prefix plain variables and preserves double underscores instead of mapping `CONFCTL_A__B` to `a.b`.

Builder failures are real, but they mostly read as fallout from the schema/store/loader contract mismatches above rather than as a separate root cause.
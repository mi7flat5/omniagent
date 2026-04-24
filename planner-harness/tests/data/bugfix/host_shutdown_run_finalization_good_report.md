Repro: `ProjectHostTest.HostDestructorFinalizesActiveRun` destroyed the host while a coordinator turn was still blocked. After shutdown, the persisted run could stay at `RunStatus::Running`, and the reloaded host would return that stale state from `get_run`.

Root cause: `ProjectEngineHost::~ProjectEngineHost` was not finalizing the active work through `shutdown()`, and `persist_run_state` also had to tolerate an expired host `weak_ptr` while the run state was being finalized.

Fix: make `ProjectEngineHost::~ProjectEngineHost` call `shutdown()` so active runs are drained during destruction, and keep the `persist_run_state` path defensive around `host.lock()` so final state persistence can complete cleanly with a valid `finished_at` timestamp.

Verification: re-ran `ProjectHostTest.HostDestructorFinalizesActiveRun` in `test_project_host`; after destruction the run status is not `RunStatus::Running`, `finished_at` is populated, and the reloaded host returns the finalized record from `get_run`.
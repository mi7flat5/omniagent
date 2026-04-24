Repro: `RuntimeHardening.RejectsConcurrentSubmitOnSameSession` blocked the first submit inside `RHBlockingProvider`, then issued a second submit on the same session while the first request was still in flight.

Root cause: `Session::submit` had no guardrail around `submit_mutex` and `submit_done`, so a second submit could enter the same session while another submit was already running.

Fix: take `submit_mutex`, check `submit_done`, flip it to false before enqueueing work, and throw `std::runtime_error("session already has an in-flight submit")` when the second submit arrives before the first one completes.

Verification: re-ran `RuntimeHardening.RejectsConcurrentSubmitOnSameSession` in `test_runtime_hardening`; the second submit now hits `EXPECT_THROW` with `runtime_error`, then after we release the provider the session completes and `session->wait` stays under `EXPECT_NO_THROW`.
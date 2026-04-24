Repro: `HttpProvider::parse_sse_line` was handling streamed tool calls where the `read_file` arguments were split across two SSE chunks. The first chunk contained `{"path":"README` and the second chunk finished it as `.md","start":1}` before `tool_calls` ended.

Root cause: the provider emitted `input_json_delta` fragments but the finish path still needed to close `open_tool_blocks` with `ContentBlockStop`. Without that close-out, the stream assembler did not reliably reconstruct the full JSON arguments from the fragmented JSON.

Fix: in the `finish_reason` branch, close every open tool block with `ContentBlockStop`, then emit the message stop events. Keep forwarding each argument fragment as `input_json_delta` so the assembler can reconstruct the full arguments JSON.

Verification: re-ran `HttpProvider.SendsToolChoiceAndAssemblesStreamedToolCalls` in `test_http_provider`; the reconstructed tool input now contains `README.md` and `start == 1`.
You are validating a bugfix writeup against a tracked source-of-truth case.

Return JSON only in this shape:

{
  "gaps": [
    {"quote": "", "question": "", "severity": "BLOCKING|COSMETIC"}
  ],
  "guesses": [
    {"what": "", "why": "", "severity": "BLOCKING|COSMETIC", "file_path": ""}
  ]
}

Rules:

- A BLOCKING gap means the writeup omits repro context, root cause, concrete fix detail, or verification that the case requires.
- A BLOCKING guess means the writeup claims success or coverage that is not supported by the case.
- Prefer short, concrete outputs.
- Use `quote` only when you can point to the exact problematic writeup text.
- Do not invent contradictions if the case does not provide enough evidence.

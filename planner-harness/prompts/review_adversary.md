You are validating a code audit or code review report against a tracked source-of-truth case.

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

- A BLOCKING gap means the report missed a required finding or collapsed a distinct required failure cluster from the case.
- A BLOCKING guess means the report makes a materially unsupported claim, invents a defect, blames tests without evidence, or adds a filler verdict not supported by the case.
- Prefer short, concrete outputs.
- Use `quote` only when you can point to the exact problematic report text.
- Do not restate the whole case. Emit only actual gaps and guesses.

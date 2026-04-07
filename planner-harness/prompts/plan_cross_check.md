These two files must work together. File A exports APIs that File B imports.

File A ({{path_a}}):
{{spec_section_a}}

File B ({{path_b}}):
{{spec_section_b}}

List any contradictions, mismatched signatures, or missing imports between them.
If they are consistent, return an empty list.

Output ONLY valid JSON, no markdown:
{"contradictions": [{"description": "...", "file_a_says": "...", "file_b_says": "..."}]}

"""Shared helpers for case-driven evaluation validators."""

import re


def normalize_text(text: str) -> str:
    """Lowercase and collapse whitespace for phrase matching."""
    return re.sub(r"\s+", " ", text.lower()).strip()


def contains_phrase(text: str, phrase: str) -> bool:
    """Return true when the normalized phrase appears in normalized text."""
    normalized_text = normalize_text(text)
    normalized_phrase = normalize_text(phrase)
    if not normalized_phrase:
        return False
    return normalized_phrase in normalized_text


def phrase_groups_covered(text: str, groups: list[list[str]]) -> bool:
    """Require at least one phrase from every group to appear in text."""
    if not groups:
        return True
    return all(any(contains_phrase(text, phrase) for phrase in group) for group in groups)


def matched_case_item_ids(text: str, items: list[dict]) -> list[str]:
    """Return IDs of case items whose required phrase groups are all covered."""
    matched: list[str] = []
    for item in items:
        if phrase_groups_covered(text, item.get("required_groups", [])):
            item_id = item.get("id")
            if isinstance(item_id, str) and item_id:
                matched.append(item_id)
    return matched


def matched_case_items(text: str, items: list[dict]) -> list[dict]:
    """Return the case items covered by the report text."""
    return [item for item in items if phrase_groups_covered(text, item.get("required_groups", []))]


def line_matches_heading(line: str, heading: str) -> bool:
    """Best-effort heading/title match for markdown or prose section labels."""
    normalized_line = normalize_text(line.lstrip("#*- "))
    normalized_heading = normalize_text(heading)
    if not normalized_heading:
        return False
    return normalized_line == normalized_heading or normalized_line.startswith(normalized_heading + ":")

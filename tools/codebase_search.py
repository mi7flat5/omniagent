#!/usr/bin/env python3
"""Codebase search MCP server for OmniAgent.

Provides semantic search over the omniagent-core and omniagent-web codebases
using Ollama nomic-embed-text embeddings stored in Qdrant.

Environment:
    QDRANT_URL      — Qdrant server URL (default: http://192.168.1.84:6333)
    OLLAMA_URL      — Ollama embedding server URL (default: http://172.19.192.1:11434)
    OLLAMA_MODEL    — Embedding model name (default: nomic-embed-text)
    COLLECTION      — Qdrant collection name (default: omniagent_repo)
"""

import json
import os
import re
import uuid
import urllib.request
import urllib.error
from datetime import datetime, timezone
from pathlib import Path

from fastmcp import FastMCP
from qdrant_client import QdrantClient
from qdrant_client.models import (
    Distance,
    PointStruct,
    VectorParams,
)

QDRANT_URL = os.environ.get("QDRANT_URL", "http://192.168.1.84:6333")
OLLAMA_URL = os.environ.get("OLLAMA_URL", "http://172.19.192.1:11434")
OLLAMA_MODEL = os.environ.get("OLLAMA_MODEL", "nomic-embed-text")
COLLECTION = os.environ.get("COLLECTION", "omniagent_repo")
VECTOR_DIM = 768

REPO_ROOT = Path(__file__).resolve().parent.parent

# File extensions to index per submodule
INDEX_EXTENSIONS = {
    "omniagent-core": {".h", ".hpp", ".cpp", ".c", ".py", ".md"},
    "omniagent-web": {".ts", ".tsx", ".js", ".css", ".md"},
}

SKIP_DIRS = {"build", "node_modules", ".git", "vcpkg_installed", "__pycache__",
             ".codehealthcache", "dist", "coverage"}

mcp = FastMCP("codebase-search")

_client: QdrantClient | None = None


# ---------------------------------------------------------------------------
# Ollama embedding
# ---------------------------------------------------------------------------

def embed_single(text: str) -> list[float]:
    url = f"{OLLAMA_URL}/api/embeddings"
    payload = json.dumps({"model": OLLAMA_MODEL, "prompt": text[:6000]}).encode()
    req = urllib.request.Request(url, data=payload,
                                headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read())["embedding"]


def embed_batch(texts: list[str]) -> list[list[float]]:
    truncated = [t[:6000] for t in texts]
    try:
        url = f"{OLLAMA_URL}/api/embed"
        payload = json.dumps({"model": OLLAMA_MODEL, "input": truncated}).encode()
        req = urllib.request.Request(url, data=payload,
                                    headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=120) as resp:
            return json.loads(resp.read())["embeddings"]
    except (urllib.error.HTTPError, KeyError):
        return [embed_single(t) for t in texts]


# ---------------------------------------------------------------------------
# Qdrant helpers
# ---------------------------------------------------------------------------

def get_client() -> QdrantClient:
    global _client
    if _client is None:
        _client = QdrantClient(url=QDRANT_URL, timeout=15)
        _ensure_collection()
    return _client


def _ensure_collection() -> None:
    assert _client is not None
    existing = [c.name for c in _client.get_collections().collections]
    if COLLECTION not in existing:
        _client.create_collection(
            collection_name=COLLECTION,
            vectors_config=VectorParams(size=VECTOR_DIM, distance=Distance.COSINE),
        )


def _get_vector_name() -> str | None:
    """Detect if collection uses named vectors (legacy FastEmbed)."""
    try:
        client = get_client()
        info = client.get_collection(COLLECTION)
        vectors_cfg = info.config.params.vectors
        if isinstance(vectors_cfg, dict):
            return next(iter(vectors_cfg))
    except Exception:
        pass
    return None


# ---------------------------------------------------------------------------
# Chunking
# ---------------------------------------------------------------------------

_CPP_BOUNDARY = re.compile(
    r"^(?:"
    r"(?:template\s*<[^>]*>\s*)?"
    r"(?:(?:static|inline|virtual|explicit|constexpr|const|override|noexcept|unsigned|signed|long|short)\s+)*"
    r"(?:\w[\w:*&<>, ]*\s+)+"
    r"\w+\s*\([^;]*$"
    r"|class\s+\w+"
    r"|struct\s+\w+"
    r"|namespace\s+\w+"
    r"|enum\s+(?:class\s+)?\w+"
    r"|TEST(?:_F|_P)?\s*\("
    r")",
    re.MULTILINE,
)


def chunk_file(path: Path, content: str) -> list[dict]:
    lines = content.splitlines(keepends=True)
    if not lines:
        return []

    suffix = path.suffix.lower()
    is_cpp = suffix in {".h", ".hpp", ".c", ".cpp"}

    if len(lines) <= 200:
        return [{
            "text": content,
            "start_line": 1,
            "end_line": len(lines),
            "chunk_type": "file",
        }]

    if is_cpp:
        return _chunk_cpp(lines)
    else:
        return _chunk_generic(lines)


def _chunk_cpp(lines: list[str]) -> list[dict]:
    boundaries = [0]
    for i, line in enumerate(lines):
        if i > 0 and _CPP_BOUNDARY.match(line):
            boundaries.append(i)

    chunks = []
    for idx in range(len(boundaries)):
        start = boundaries[idx]
        end = boundaries[idx + 1] if idx + 1 < len(boundaries) else len(lines)
        text = "".join(lines[start:end])
        if text.strip():
            chunks.append({
                "text": text,
                "start_line": start + 1,
                "end_line": end,
                "chunk_type": "function" if idx > 0 else "header",
            })

    # Merge tiny chunks, split huge ones
    return _normalize_chunks(chunks, lines)


def _chunk_generic(lines: list[str]) -> list[dict]:
    target = 150
    overlap = 20
    chunks = []
    i = 0
    while i < len(lines):
        end = min(i + target, len(lines))
        text = "".join(lines[i:end])
        if text.strip():
            chunks.append({
                "text": text,
                "start_line": i + 1,
                "end_line": end,
                "chunk_type": "block",
            })
        i = end - overlap if end < len(lines) else len(lines)
    return chunks


def _normalize_chunks(chunks: list[dict], _lines: list[str]) -> list[dict]:
    max_lines = 200
    result = []
    for chunk in chunks:
        if len(chunk["text"].splitlines()) > max_lines:
            sub_lines = chunk["text"].splitlines(keepends=True)
            base = chunk["start_line"]
            for j in range(0, len(sub_lines), max_lines):
                sub = sub_lines[j:j + max_lines]
                text = "".join(sub)
                if text.strip():
                    result.append({
                        "text": text,
                        "start_line": base + j,
                        "end_line": base + j + len(sub),
                        "chunk_type": chunk["chunk_type"],
                    })
        elif len(chunk["text"].splitlines()) < 10 and result:
            # Merge tiny chunk into previous
            prev = result[-1]
            prev["text"] += chunk["text"]
            prev["end_line"] = chunk["end_line"]
        else:
            result.append(chunk)
    return result


# ---------------------------------------------------------------------------
# MCP Tools
# ---------------------------------------------------------------------------

@mcp.tool()
def search_codebase(query: str, limit: int = 10) -> list[dict]:
    """Search the OmniAgent codebase semantically.

    Searches indexed source code from both omniagent-core (C++) and
    omniagent-web (TypeScript/Preact) for relevant code, functions,
    classes, and documentation.

    Use this to find:
    - How a feature is implemented
    - Where a class/function/type is defined
    - Code patterns and conventions
    - Architecture and data flow

    Args:
        query: Natural language or code search query.
        limit: Maximum results to return (default 10).

    Returns:
        List of matching code chunks with file path, line range, score, and content.
    """
    try:
        client = get_client()
        query_vec = embed_single(query)
        vec_name = _get_vector_name()

        results = client.query_points(
            collection_name=COLLECTION,
            query=query_vec,
            using=vec_name,
            limit=limit,
            with_payload=True,
        )

        out = []
        for hit in results.points:
            p = hit.payload or {}
            out.append({
                "file_path": p.get("file_path", ""),
                "start_line": p.get("start_line", 0),
                "end_line": p.get("end_line", 0),
                "chunk_type": p.get("chunk_type", ""),
                "score": hit.score,
                "text": p.get("document", p.get("text", "")),
            })
        return out
    except Exception as e:
        return [{"error": str(e)}]


@mcp.tool()
def index_codebase() -> dict:
    """Re-index the OmniAgent codebase into the vector database.

    Walks omniagent-core and omniagent-web, chunks source files, embeds
    them via Ollama nomic-embed-text, and upserts into Qdrant.

    This replaces all existing indexed data in the collection.

    Returns:
        Stats: files processed, chunks created, errors.
    """
    try:
        client = get_client()

        # Clear existing points
        try:
            client.delete_collection(COLLECTION)
        except Exception:
            pass
        client.create_collection(
            collection_name=COLLECTION,
            vectors_config=VectorParams(size=VECTOR_DIM, distance=Distance.COSINE),
        )

        files_processed = 0
        chunks_created = 0
        errors = 0
        batch_points: list[PointStruct] = []
        batch_size = 20

        def flush_batch():
            nonlocal chunks_created, errors
            if not batch_points:
                return
            try:
                client.upsert(collection_name=COLLECTION, points=list(batch_points))
                chunks_created += len(batch_points)
            except Exception:
                errors += len(batch_points)
            batch_points.clear()

        for submodule, extensions in INDEX_EXTENSIONS.items():
            submod_path = REPO_ROOT / submodule
            if not submod_path.is_dir():
                continue

            for file_path in sorted(submod_path.rglob("*")):
                if not file_path.is_file():
                    continue
                if file_path.suffix.lower() not in extensions:
                    continue
                if any(d in file_path.parts for d in SKIP_DIRS):
                    continue
                if file_path.stat().st_size > 512 * 1024:
                    continue

                try:
                    content = file_path.read_text(encoding="utf-8", errors="replace")
                except Exception:
                    errors += 1
                    continue

                rel_path = str(file_path.relative_to(REPO_ROOT))
                chunks = chunk_file(file_path, content)
                files_processed += 1

                for chunk in chunks:
                    try:
                        vec = embed_single(chunk["text"])
                    except Exception:
                        errors += 1
                        continue

                    batch_points.append(PointStruct(
                        id=str(uuid.uuid4()),
                        vector=vec,
                        payload={
                            "document": chunk["text"],
                            "file_path": rel_path,
                            "start_line": chunk["start_line"],
                            "end_line": chunk["end_line"],
                            "chunk_type": chunk["chunk_type"],
                            "indexed_at": datetime.now(timezone.utc).isoformat(),
                        },
                    ))

                    if len(batch_points) >= batch_size:
                        flush_batch()

        flush_batch()

        return {
            "status": "indexed",
            "files_processed": files_processed,
            "chunks_created": chunks_created,
            "errors": errors,
            "collection": COLLECTION,
        }
    except Exception as e:
        return {"error": str(e)}


if __name__ == "__main__":
    mcp.run()

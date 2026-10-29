"""Small transactional cache for deterministic per-function emitted C bodies.

The caller owns discovery and input-fingerprint validation. Cache misses are
written to a private pending SQLite file and become visible only when
``publish`` receives a successful input guard. The existing cache is never
modified in place.
"""
from __future__ import annotations

import hashlib
import json
import os
import sqlite3
import sys
import time
import uuid
import warnings
from pathlib import Path
from typing import Callable
from urllib.parse import quote

sys.dont_write_bytecode = True

_FORMAT_VERSION = "ac1-emission-cache-v1"


class _GuardCallbackRaised(Exception):
    def __init__(self, error: BaseException):
        super().__init__(str(error))
        self.error = error
        self.traceback = error.__traceback__


def _canonical_json(value) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _sqlite_uri(path: Path, mode: str) -> str:
    return "file:" + quote(path.resolve().as_posix(), safe="/:\\") + "?mode=" + mode


def _body_hash(body: str) -> str:
    return _sha256_bytes(body.encode("utf-8"))


def _function_key(stamp: str, spec: dict) -> str:
    # Depth only determines discovery reachability. It is not consulted by the
    # body emitter; every other spec field remains part of the key.
    semantic_spec = dict(spec)
    semantic_spec.pop("depth", None)
    payload = stamp + "\0" + _canonical_json(semantic_spec)
    return _sha256_bytes(payload.encode("utf-8"))


class EmissionCache:
    """Cache emitted function bodies and publish misses after a caller guard.

    Args:
        path: Persistent SQLite file, normally under the ignored ``work/``.
        semantic_inputs: JSON-serializable binary/tool/version fingerprint.
            Do not include startup seeds, depth limits, or the discovered set.

    ``get_or_lift`` accepts the complete function spec and a zero-argument
    callable that emits its body. ``publish`` calls the supplied guard before
    staging and again immediately before atomic replacement. A false guard
    discards pending rows and preserves the active database.
    """

    def __init__(self, path: str | os.PathLike, semantic_inputs):
        self.path = Path(path).resolve()
        self._reader: sqlite3.Connection | None = None
        self._pending: sqlite3.Connection | None = None
        self._pending_path: Path | None = None
        self._pending_rows = 0
        self._stats = {
            "hits": 0,
            "misses": 0,
            "corrupt_rows": 0,
            "foreign_cache": 0,
            "read_errors": 0,
            "write_errors": 0,
            "pending_hits": 0,
            "pending_rows": 0,
            "lookup_seconds": 0.0,
            "lift_seconds": 0.0,
            "publish_seconds": 0.0,
            "publish_status": "not-published",
        }
        self._write_disabled = False
        self._warned_io = False
        self._cache_available = True
        self._module_hash: str | None = None
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
        except OSError:
            self._cache_available = False
            self._stats["write_errors"] += 1
            self._warn_io("cache directory unavailable; continuing without cache")
        try:
            self._module_hash = _sha256_bytes(Path(__file__).read_bytes())
        except OSError:
            self._cache_available = False
            self._stats["read_errors"] += 1
            self._warn_io("cache module fingerprint unavailable; continuing without cache")
        stamp_payload = {
            "format": _FORMAT_VERSION,
            "module_sha256": self._module_hash or "unavailable",
            "semantic_inputs": semantic_inputs,
        }
        self.semantic_stamp = _sha256_bytes(_canonical_json(stamp_payload).encode("utf-8"))
        if self._cache_available:
            self._open_reader()

    @property
    def stats(self) -> dict:
        result = dict(self._stats)
        result["pending_rows"] = self._pending_rows
        return result

    def _open_reader(self) -> None:
        if not self._cache_available or not self.path.is_file():
            return
        try:
            db = sqlite3.connect(_sqlite_uri(self.path, "ro"), uri=True)
            metadata = dict(db.execute("SELECT key, value FROM metadata"))
            if (metadata.get("format") != _FORMAT_VERSION or
                    metadata.get("semantic_stamp") != self.semantic_stamp):
                self._stats["foreign_cache"] += 1
                db.close()
                return
            db.execute("SELECT function_key, body, body_sha256 FROM bodies LIMIT 0")
            self._reader = db
        except (sqlite3.DatabaseError, OSError, ValueError):
            self._stats["foreign_cache"] += 1
            self._stats["read_errors"] += 1
            self._warn_io("cache read failed; treating the cache as empty")
            try:
                db.close()
            except (UnboundLocalError, sqlite3.DatabaseError):
                pass

    def _ensure_pending(self) -> sqlite3.Connection:
        if self._pending is None:
            self._pending_path = self.path.with_name(
                self.path.name + ".pending." + uuid.uuid4().hex
            )
            db = sqlite3.connect(self._pending_path)
            db.execute("PRAGMA synchronous=FULL")
            _create_schema(db, self.semantic_stamp)
            self._pending = db
        return self._pending

    def _lookup_pending(self, key: str) -> str | None:
        if self._pending is None:
            return None
        try:
            row = self._pending.execute(
                "SELECT body, body_sha256 FROM bodies WHERE function_key=?", (key,)
            ).fetchone()
        except sqlite3.DatabaseError:
            return None
        if row is None:
            return None
        body, expected_hash = row
        if (not isinstance(body, str) or not isinstance(expected_hash, str) or
                _body_hash(body) != expected_hash):
            self._stats["corrupt_rows"] += 1
            return None
        return body

    def get_or_lift(self, spec: dict, lift_body: Callable[[], str]) -> str:
        """Return a verified cached body or run ``lift_body`` and stage it."""
        if not self._cache_available:
            self._stats["misses"] += 1
            lift_start = time.perf_counter()
            body = lift_body()
            self._stats["lift_seconds"] += time.perf_counter() - lift_start
            if not isinstance(body, str):
                raise TypeError("emission cache lift callback must return str")
            return body
        start = time.perf_counter()
        key = _function_key(self.semantic_stamp, spec)
        pending = self._lookup_pending(key)
        if pending is not None:
            self._stats["pending_hits"] += 1
            self._stats["lookup_seconds"] += time.perf_counter() - start
            return pending
        if self._reader is not None:
            try:
                row = self._reader.execute(
                    "SELECT body, body_sha256 FROM bodies WHERE function_key=?", (key,)
                ).fetchone()
            except sqlite3.DatabaseError:
                self._stats["corrupt_rows"] += 1
                self._reader.close()
                self._reader = None
                row = None
            if row is not None:
                body, expected_hash = row
                if (isinstance(body, str) and isinstance(expected_hash, str) and
                        _body_hash(body) == expected_hash):
                    self._stats["hits"] += 1
                    self._stats["lookup_seconds"] += time.perf_counter() - start
                    return body
                self._stats["corrupt_rows"] += 1

        self._stats["misses"] += 1
        self._stats["lookup_seconds"] += time.perf_counter() - start
        lift_start = time.perf_counter()
        body = lift_body()
        self._stats["lift_seconds"] += time.perf_counter() - lift_start
        if not isinstance(body, str):
            raise TypeError("emission cache lift callback must return str")
        if not self._write_disabled:
            try:
                db = self._ensure_pending()
                db.execute(
                    "INSERT OR REPLACE INTO bodies(function_key, body, body_sha256) VALUES (?, ?, ?)",
                    (key, body, _body_hash(body)),
                )
                self._pending_rows += 1
            except (sqlite3.Error, OSError):
                self._stats["write_errors"] += 1
                self._warn_io("cache staging failed; continuing without cache writes")
                self._write_disabled = True
                self.discard()
        return body

    def publish(self, input_guard: Callable[[], bool]) -> bool:
        """Merge staged rows and atomically publish iff ``input_guard`` holds.

        Any failed guard or staging error leaves the active cache untouched.
        A corrupt, foreign, or stale active database is treated as empty and is
        replaced only after a successful guard and integrity check.
        """
        start = time.perf_counter()
        try:
            if not input_guard():
                self.discard()
                self._stats["publish_status"] = "guard-failed"
                self._stats["publish_seconds"] += time.perf_counter() - start
                return False
        except BaseException:
            self.discard()
            self._stats["publish_status"] = "guard-error"
            raise
        if not self._cache_available:
            self._stats["publish_status"] = "io-disabled"
            self._stats["publish_seconds"] += time.perf_counter() - start
            return False
        if not self._module_unchanged():
            self.discard()
            self._stats["publish_status"] = "module-changed"
            self._stats["publish_seconds"] += time.perf_counter() - start
            return False
        if self._pending is None or self._pending_rows == 0:
            self._stats["publish_status"] = "no-pending"
            self._stats["publish_seconds"] += time.perf_counter() - start
            return True

        self._close_reader()
        stage_path = self.path.with_name(self.path.name + ".stage." + uuid.uuid4().hex)
        stage_db: sqlite3.Connection | None = None
        source_db: sqlite3.Connection | None = None
        cleanup_stage = False
        try:
            stage_db = sqlite3.connect(stage_path)
            stage_db.execute("PRAGMA synchronous=FULL")
            source_db = self._compatible_source_for_merge()
            if source_db is not None:
                source_db.backup(stage_db)
                source_db.close()
                source_db = None
            else:
                _create_schema(stage_db, self.semantic_stamp)

            # Recheck metadata after backup. A foreign/corrupt source is never
            # allowed to smuggle rows into a replacement database.
            metadata = dict(stage_db.execute("SELECT key, value FROM metadata"))
            if (metadata.get("format") != _FORMAT_VERSION or
                    metadata.get("semantic_stamp") != self.semantic_stamp):
                raise sqlite3.DatabaseError("staged cache metadata mismatch")
            stage_db.commit()
            stage_db.execute("BEGIN IMMEDIATE")
            # SQLite cannot SELECT across connections without ATTACH; copy rows
            # explicitly in bounded batches to keep memory use predictable.
            self._pending.commit()
            rows = self._pending.execute(
                "SELECT function_key, body, body_sha256 FROM bodies"
            )
            while True:
                batch = rows.fetchmany(128)
                if not batch:
                    break
                stage_db.executemany(
                    "INSERT OR REPLACE INTO bodies(function_key, body, body_sha256) VALUES (?, ?, ?)",
                    batch,
                )
            stage_db.commit()
            check = stage_db.execute("PRAGMA integrity_check").fetchone()
            if not check or check[0] != "ok":
                raise sqlite3.DatabaseError("staged cache integrity check failed")
            stage_db.close()
            stage_db = None
            try:
                guard_passed = input_guard()
            except BaseException as exc:
                raise _GuardCallbackRaised(exc) from None
            if not guard_passed:
                cleanup_stage = True
                self.discard()
                self._stats["publish_status"] = "guard-failed"
                self._stats["publish_seconds"] += time.perf_counter() - start
                return False
            if not self._module_unchanged():
                cleanup_stage = True
                self.discard()
                self._stats["publish_status"] = "module-changed"
                self._stats["publish_seconds"] += time.perf_counter() - start
                return False
            os.replace(stage_path, self.path)
            self._discard_pending_file()
            self._pending_rows = 0
            self._stats["publish_status"] = "published"
            self._stats["publish_seconds"] += time.perf_counter() - start
            return True
        except _GuardCallbackRaised as wrapped:
            self._stats["publish_seconds"] += time.perf_counter() - start
            self._stats["publish_status"] = "guard-error"
            cleanup_stage = True
            self.discard()
            raise wrapped.error.with_traceback(wrapped.traceback) from None
        except (sqlite3.Error, OSError):
            self._stats["publish_seconds"] += time.perf_counter() - start
            self._stats["write_errors"] += 1
            self._stats["publish_status"] = "io-failed"
            cleanup_stage = True
            self.discard()
            self._warn_io("cache publish failed; generated output remains valid and the prior cache is preserved")
            return False
        except Exception:
            self._stats["publish_seconds"] += time.perf_counter() - start
            self._stats["publish_status"] = "error"
            cleanup_stage = True
            self.discard()
            raise
        finally:
            if source_db is not None:
                source_db.close()
            if stage_db is not None:
                stage_db.close()
            if cleanup_stage:
                self._remove_sqlite_files(stage_path)

    def _module_unchanged(self) -> bool:
        if self._module_hash is None:
            return False
        try:
            return _sha256_bytes(Path(__file__).read_bytes()) == self._module_hash
        except OSError:
            self._stats["read_errors"] += 1
            self._warn_io("cache module fingerprint read failed; generated output is uncached")
            return False

    def _compatible_source_for_merge(self) -> sqlite3.Connection | None:
        if not self.path.is_file():
            return None
        try:
            source = sqlite3.connect(_sqlite_uri(self.path, "ro"), uri=True)
            metadata = dict(source.execute("SELECT key, value FROM metadata"))
            source.execute("SELECT function_key, body, body_sha256 FROM bodies LIMIT 0")
            if (metadata.get("format") != _FORMAT_VERSION or
                    metadata.get("semantic_stamp") != self.semantic_stamp):
                source.close()
                return None
            return source
        except (sqlite3.DatabaseError, OSError, ValueError):
            try:
                source.close()
            except (UnboundLocalError, sqlite3.DatabaseError):
                pass
            return None

    def _close_reader(self) -> None:
        if self._reader is not None:
            self._reader.close()
            self._reader = None

    def _warn_io(self, message: str) -> None:
        if not self._warned_io:
            warnings.warn(message, RuntimeWarning, stacklevel=2)
            self._warned_io = True

    def _discard_pending_file(self) -> None:
        if self._pending is not None:
            self._pending.close()
            self._pending = None
        if self._pending_path is not None:
            self._remove_sqlite_files(self._pending_path)
            self._pending_path = None

    @staticmethod
    def _remove_sqlite_files(path: Path) -> None:
        for candidate in (path, Path(str(path) + "-journal"), Path(str(path) + "-wal"),
                          Path(str(path) + "-shm")):
            try:
                candidate.unlink(missing_ok=True)
            except OSError:
                pass

    def discard(self) -> None:
        """Discard unpublished pending rows; preserve the active cache."""
        self._discard_pending_file()
        self._pending_rows = 0

    def close(self) -> None:
        """Close this cache without publishing pending rows."""
        self._close_reader()
        self.discard()

    def __enter__(self) -> "EmissionCache":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()


def _create_schema(db: sqlite3.Connection, stamp: str) -> None:
    db.execute("CREATE TABLE metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL)")
    db.executemany("INSERT INTO metadata(key, value) VALUES (?, ?)", (
        ("format", _FORMAT_VERSION),
        ("semantic_stamp", stamp),
    ))
    db.execute(
        "CREATE TABLE bodies (function_key TEXT PRIMARY KEY, body TEXT NOT NULL, "
        "body_sha256 TEXT NOT NULL)"
    )


def _selftest() -> None:
    import tempfile

    root = Path(__file__).resolve().parents[1]
    work = root / "work"
    work.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="emission_cache_test_", dir=work) as temp:
        path = Path(temp) / "bodies.sqlite3"
        spec = {"address": 0x401000, "size": 16, "depth": 1,
                "blocks": [{"start": 0x401000, "instructions": [[0x401000, 1]]}]}
        calls = {"body": 0}

        def lift():
            calls["body"] += 1
            return "void lifted_body(void) { return; }\n"

        cache = EmissionCache(path, {"binary": "b" * 64, "driver": "d" * 64})
        expected = "void lifted_body(void) { return; }\n"
        assert cache.get_or_lift(spec, lift) == expected and calls["body"] == 1
        assert cache.publish(lambda: True)
        cache.close()

        warm = EmissionCache(path, {"driver": "d" * 64, "binary": "b" * 64})
        depth_changed = dict(spec, depth=99)
        assert warm.get_or_lift(depth_changed, lift) == expected and calls["body"] == 1
        changed_cfg = dict(depth_changed, size=17)
        assert warm.get_or_lift(changed_cfg, lift) == expected and calls["body"] == 2
        warm.close()

        # A bad content hash is a miss and recomputes through the callback.
        corrupt_db = sqlite3.connect(path)
        try:
            corrupt_db.execute("UPDATE bodies SET body='corrupt' WHERE body_sha256=?", (_body_hash(expected),))
            corrupt_db.commit()
        finally:
            corrupt_db.close()
        corrupt = EmissionCache(path, {"binary": "b" * 64, "driver": "d" * 64})
        assert corrupt.get_or_lift(spec, lift) == expected and calls["body"] == 3
        corrupt.discard()
        corrupt.close()

        # SQLite's dynamic typing can return an integer despite a TEXT column.
        corrupt_db = sqlite3.connect(path)
        try:
            corrupt_db.execute("UPDATE bodies SET body=123 WHERE body='corrupt'")
            corrupt_db.commit()
        finally:
            corrupt_db.close()
        malformed = EmissionCache(path, {"binary": "b" * 64, "driver": "d" * 64})
        assert malformed.get_or_lift(spec, lift) == expected and calls["body"] == 4
        malformed.discard()
        malformed.close()

        # A failed guard must leave the active database unchanged and pending
        # rows unavailable to a later cache instance.
        before = path.read_bytes()
        pending = EmissionCache(path, {"binary": "b" * 64, "driver": "d" * 64})
        new_spec = dict(spec, address=0x401100)
        assert pending.get_or_lift(new_spec, lift) == expected and calls["body"] == 5
        guard_calls = {"count": 0}

        def fail_after_staging():
            guard_calls["count"] += 1
            return guard_calls["count"] == 1

        assert pending.publish(fail_after_staging) is False
        assert guard_calls["count"] == 2
        assert path.read_bytes() == before
        assert not list(Path(temp).glob("bodies.sqlite3.stage.*"))
        pending.close()
        check = EmissionCache(path, {"binary": "b" * 64, "driver": "d" * 64})
        new_calls = {"body": 0}
        assert check.get_or_lift(new_spec, lambda: new_calls.__setitem__("body", 1) or expected) == expected
        assert new_calls["body"] == 1
        check.close()

        for error in (OSError("guard I/O failure"), sqlite3.OperationalError("guard DB failure")):
            raising = EmissionCache(path, {"binary": "b" * 64, "driver": "d" * 64})
            raised_spec = dict(spec, address=0x401200 + calls["body"])
            assert raising.get_or_lift(raised_spec, lift) == expected
            error_guard_calls = {"count": 0}

            def raise_after_staging():
                error_guard_calls["count"] += 1
                if error_guard_calls["count"] == 1:
                    return True
                raise error

            try:
                raising.publish(raise_after_staging)
            except type(error):
                pass
            else:
                raise AssertionError("input guard exception was swallowed")
            assert path.read_bytes() == before
            assert not list(Path(temp).glob("bodies.sqlite3.stage.*"))
            raising.close()

        # An unusable cache path warns and leaves lifting operational.
        invalid_parent = Path(temp) / "not_a_directory"
        invalid_parent.write_text("file", encoding="utf-8")
        with warnings.catch_warnings(record=True):
            disabled = EmissionCache(invalid_parent / "bodies.sqlite3", {"binary": "b" * 64})
        disabled_calls = {"body": 0}
        assert disabled.get_or_lift(spec, lambda: disabled_calls.__setitem__("body", 1) or expected) == expected
        assert disabled_calls["body"] == 1
        assert disabled.publish(lambda: True) is False
        disabled.close()

        # A foreign semantic stamp is a miss; it is not read as valid output.
        foreign = EmissionCache(path, {"binary": "different"})
        foreign_calls = {"body": 0}
        assert foreign.get_or_lift(spec, lambda: foreign_calls.__setitem__("body", 1) or expected) == expected
        assert foreign_calls["body"] == 1
        foreign.discard()
        foreign.close()
    print("emission cache selftest passed")


if __name__ == "__main__":
    import sys
    sys.dont_write_bytecode = True
    if sys.argv[1:] == ["--selftest"]:
        _selftest()

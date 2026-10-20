#!/usr/bin/env python3
"""Focused IMG-005A.0 migration, rollback, preservation and restore evidence."""

import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path


def run(executable, directory, success=True):
    result = subprocess.run(
        [executable, "--data-dir", str(directory), "--migrate-only"],
        text=True, capture_output=True, timeout=10,
    )
    if success and result.returncode != 0:
        raise AssertionError(result.stderr)
    if not success and result.returncode == 0:
        raise AssertionError("migration unexpectedly succeeded")
    return result


def legacy_database(path, interrupt=False):
    db = sqlite3.connect(path)
    db.executescript("""
        PRAGMA foreign_keys=ON;
        CREATE TABLE images(
          id TEXT PRIMARY KEY, filename TEXT NOT NULL, sample_id TEXT NOT NULL, capture_date TEXT NOT NULL,
          created_at TEXT NOT NULL, sha256 TEXT NOT NULL UNIQUE, bytes INTEGER NOT NULL,
          width INTEGER NOT NULL, height INTEGER NOT NULL, format TEXT NOT NULL,
          brightness REAL NOT NULL, qc_state TEXT NOT NULL, pipeline TEXT NOT NULL,
          original_path TEXT NOT NULL, preview_path TEXT NOT NULL, grayscale_path TEXT NOT NULL,
          user_email TEXT NOT NULL DEFAULT ''
        );
        CREATE TABLE events(id INTEGER PRIMARY KEY AUTOINCREMENT,image_id TEXT,event TEXT NOT NULL,at TEXT NOT NULL,detail TEXT NOT NULL);
        CREATE TABLE users(id TEXT PRIMARY KEY,email TEXT NOT NULL UNIQUE,name TEXT NOT NULL,created_at TEXT NOT NULL,last_login_at TEXT NOT NULL);
        CREATE TABLE auth_codes(id TEXT PRIMARY KEY,email TEXT NOT NULL,code TEXT NOT NULL,code_hash TEXT NOT NULL,expires_at TEXT NOT NULL,used_at TEXT,created_at TEXT NOT NULL);
        CREATE TABLE sessions(token TEXT PRIMARY KEY,user_id TEXT NOT NULL REFERENCES users(id),email TEXT NOT NULL,created_at TEXT NOT NULL,expires_at TEXT NOT NULL);
        INSERT INTO users VALUES('user-1','cientista@embrapa.br','Cientista','2026-10-01T00:00:00Z','2026-10-02T00:00:00Z');
        INSERT INTO sessions VALUES('session-1','user-1','cientista@embrapa.br','2026-10-02T00:00:00Z','2027-10-02T00:00:00Z');
        INSERT INTO images VALUES('image-1','raiz.tif','A1','2026-09-30','2026-10-01T00:00:00Z','abc',123,10,12,'tiff',100.0,'REVIEW','legacy','blobs/a','derived/a','derived/b','cientista@embrapa.br');
    """)
    if interrupt:
        db.executescript("""
            CREATE TRIGGER interrupt_migration BEFORE UPDATE ON images
            BEGIN SELECT RAISE(ABORT,'intentional migration interruption'); END;
        """)
    db.commit()
    db.close()


def snapshot(path):
    db = sqlite3.connect(path)
    result = {
        "migration": db.execute("SELECT version,name,checksum FROM schema_migrations ORDER BY version").fetchall(),
        "users": db.execute("SELECT * FROM users").fetchall(),
        "sessions": db.execute("SELECT * FROM sessions").fetchall(),
        "images": db.execute("SELECT id,user_email,user_id,original_path FROM images").fetchall(),
        "identity_links": db.execute("SELECT * FROM identity_links").fetchall(),
        "identity_audit_events": db.execute("SELECT * FROM identity_audit_events").fetchall(),
        "integrity": db.execute("PRAGMA integrity_check").fetchone()[0],
        "foreign_keys": db.execute("PRAGMA foreign_key_check").fetchall(),
    }
    db.close()
    return result


def main():
    executable = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="rizoma-migrations-") as temp_name:
        root = Path(temp_name)

        # Empty database initialization and repeatability.
        empty = root / "empty"
        run(executable, empty)
        first = snapshot(empty / "morfo.sqlite3")
        run(executable, empty)
        assert snapshot(empty / "morfo.sqlite3") == first
        assert first["integrity"] == "ok" and not first["foreign_keys"]

        # Representative v0.2.0 legacy database and preservation of links.
        legacy = root / "legacy"
        legacy.mkdir()
        legacy_database(legacy / "morfo.sqlite3")
        run(executable, legacy)
        preserved = snapshot(legacy / "morfo.sqlite3")
        assert preserved["images"] == [("image-1", "cientista@embrapa.br", "user-1", "blobs/a")]
        assert len(preserved["users"]) == len(preserved["sessions"]) == 1

        # A failure after ALTER TABLE must roll back both schema and history.
        interrupted = root / "interrupted"
        interrupted.mkdir()
        interrupted_db = interrupted / "morfo.sqlite3"
        legacy_database(interrupted_db, interrupt=True)
        run(executable, interrupted, success=False)
        db = sqlite3.connect(interrupted_db)
        assert "user_id" not in {row[1] for row in db.execute("PRAGMA table_info(images)")}
        assert not db.execute("SELECT 1 FROM sqlite_schema WHERE name='schema_migrations'").fetchone()
        db.execute("DROP TRIGGER interrupt_migration")
        db.commit(); db.close()
        run(executable, interrupted)
        assert snapshot(interrupted_db)["images"][0][2] == "user-1"

        # Tampered history, future versions and incompatible legacy shapes fail closed.
        tampered = root / "tampered"
        tampered.mkdir()
        legacy_database(tampered / "morfo.sqlite3")
        run(executable, tampered)
        db = sqlite3.connect(tampered / "morfo.sqlite3")
        db.execute("UPDATE schema_migrations SET checksum='tampered' WHERE version=1")
        db.commit(); db.close()
        run(executable, tampered, success=False)

        tampered5 = root / "tampered5"
        tampered5.mkdir()
        legacy_database(tampered5 / "morfo.sqlite3")
        run(executable, tampered5)
        db = sqlite3.connect(tampered5 / "morfo.sqlite3")
        db.execute("UPDATE schema_migrations SET checksum='tampered' WHERE version=5")
        db.commit(); db.close()
        run(executable, tampered5, success=False)

        future = root / "future"
        run(executable, future)
        db = sqlite3.connect(future / "morfo.sqlite3")
        db.execute("INSERT INTO schema_migrations VALUES(99,'future','future','2026-01-01T00:00:00Z')")
        db.commit(); db.close()
        run(executable, future, success=False)

        incompatible = root / "incompatible"
        incompatible.mkdir()
        db = sqlite3.connect(incompatible / "morfo.sqlite3")
        db.execute("CREATE TABLE images(id TEXT PRIMARY KEY)")
        db.commit(); db.close()
        run(executable, incompatible, success=False)

        broken_fk = root / "broken-foreign-key"
        run(executable, broken_fk)
        db = sqlite3.connect(broken_fk / "morfo.sqlite3")
        db.execute("PRAGMA foreign_keys=OFF")
        db.execute("INSERT INTO sessions VALUES('orphan','missing-user','orphan@embrapa.br','2026-01-01T00:00:00Z','2027-01-01T00:00:00Z')")
        db.commit(); db.close()
        run(executable, broken_fk, success=False)

        # Consistent online backup and effective isolated restore.
        backup = root / "backup.sqlite3"
        source = sqlite3.connect(legacy / "morfo.sqlite3")
        destination = sqlite3.connect(backup)
        source.backup(destination)
        destination.close(); source.close()
        restored = root / "restored"
        restored.mkdir()
        restored_db = sqlite3.connect(restored / "morfo.sqlite3")
        source = sqlite3.connect(backup)
        source.backup(restored_db)
        restored_db.close(); source.close()
        run(executable, restored)
        assert snapshot(restored / "morfo.sqlite3") == preserved

        print("MORFO migrations PASS: clean install, legacy, repeat, rollback, compatibility, backup restore")


if __name__ == "__main__":
    main()

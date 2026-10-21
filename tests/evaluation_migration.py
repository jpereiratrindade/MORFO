#!/usr/bin/env python3
import sqlite3, subprocess, sys, tempfile
from pathlib import Path

with tempfile.TemporaryDirectory(prefix="morfo-evaluate-") as name:
    root = Path(name)
    subprocess.run([sys.argv[1], "--data-dir", str(root), "--migrate-only"], check=True)
    db = sqlite3.connect(root / "morfo.sqlite3")
    db.execute("INSERT INTO users(id,email,name,created_at,last_login_at) VALUES('u','u@example.org','U','t','t')")
    db.execute("INSERT INTO evaluation_protocols VALUES('p','u','EXP-003 baseline','h','SEGMENTATION','plant','grouped','{}','none','FROZEN',1,'t')")
    db.execute("INSERT INTO evaluation_datasets VALUES('d','p','test','sha','SPECIALIST','t','t')")
    db.execute("INSERT INTO evaluation_units VALUES('a','d','a','DEVELOPMENT','plant-1','sha-a','{}')")
    try:
        db.execute("INSERT INTO evaluation_units VALUES('b','d','b','TEST','plant-1','sha-b','{}')")
        raise AssertionError("group leakage accepted")
    except sqlite3.IntegrityError as error:
        assert "cannot cross" in str(error)
    db.execute("INSERT INTO evaluation_units VALUES('b','d','b','TEST','plant-2','sha-b','{}')")
    db.execute("INSERT INTO evaluation_runs VALUES('r','p','d','u','EXP-003','003','rev','{}','{}',42,'RUNNING','t',NULL,NULL)")
    try:
        db.execute("INSERT INTO evaluation_observations VALUES('o','r','b','dice',0.9,NULL,'{}','t')")
        raise AssertionError("accuracy without reference accepted")
    except sqlite3.IntegrityError as error:
        assert "requires independent reference" in str(error)
    db.execute("INSERT INTO evaluation_observations VALUES('o','r','b','dice',0.9,'ref-sha','{}','t')")
    try:
        db.execute("UPDATE evaluation_observations SET value=1 WHERE id='o'")
        raise AssertionError("observation overwrite accepted")
    except sqlite3.IntegrityError as error:
        assert "immutable" in str(error)
    assert db.execute("SELECT version,name FROM schema_migrations WHERE version=6").fetchone()[1].startswith("morfo-dev-002")
    print("MORFO-AVALIA migration PASS: immutable evidence, independent reference, no split leakage")

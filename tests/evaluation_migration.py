#!/usr/bin/env python3
import sqlite3, subprocess, sys, tempfile
from pathlib import Path

with tempfile.TemporaryDirectory(prefix="morfo-evaluate-") as name:
    root = Path(name)
    subprocess.run([sys.argv[1], "--data-dir", str(root), "--migrate-only"], check=True)
    db = sqlite3.connect(root / "morfo.sqlite3")
    db.execute("INSERT INTO users(id,email,name,created_at,last_login_at) VALUES('u','u@example.org','U','t','t')")
    db.execute("INSERT INTO evaluation_protocols VALUES('p','u','EXP-003 baseline','h','SEGMENTATION','plant','grouped','{}','none','FROZEN',1,'t')")
    db.execute("INSERT INTO evaluation_datasets(id,protocol_id,name,content_sha256,reference_kind,frozen_at,created_at) VALUES('d','p','test','sha','SPECIALIST','t','t')")
    db.execute("INSERT INTO evaluation_units(id,dataset_id,external_key,split,group_key,input_sha256,metadata_json) VALUES('a','d','a','DEVELOPMENT','plant-1','sha-a','{}')")
    try:
        db.execute("INSERT INTO evaluation_units(id,dataset_id,external_key,split,group_key,input_sha256,metadata_json) VALUES('b','d','b','TEST','plant-1','sha-b','{}')")
        raise AssertionError("group leakage accepted")
    except sqlite3.IntegrityError as error:
        assert "cannot cross" in str(error)
    db.execute("INSERT INTO evaluation_units(id,dataset_id,external_key,split,group_key,input_sha256,metadata_json) VALUES('b','d','b','TEST','plant-2','sha-b','{}')")
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
    try:
        db.execute("UPDATE evaluation_units SET group_key='changed' WHERE id='a'")
        raise AssertionError("frozen unit overwrite accepted")
    except sqlite3.IntegrityError as error:
        assert "immutable" in str(error)
    db.execute("INSERT INTO evaluation_protocols VALUES('pd','u','draft','h','SEGMENTATION','plant','grouped','{}','none','DRAFT',1,'t')")
    db.execute("INSERT INTO evaluation_datasets(id,protocol_id,name,content_sha256,reference_kind,frozen_at,created_at,state) VALUES('dd','pd','draft','sha','NONE','','t','DRAFT')")
    db.execute("INSERT INTO evaluation_units(id,dataset_id,external_key,split,group_key,input_sha256,metadata_json) VALUES('ud','dd','ud','DEVELOPMENT','plant-d','sha-d','{}')")
    db.execute("UPDATE evaluation_units SET group_key='plant-edited',reference_sha256='ref' WHERE id='ud'")
    assert db.execute("SELECT group_key,reference_sha256 FROM evaluation_units WHERE id='ud'").fetchone()==('plant-edited','ref')
    db.execute("DELETE FROM evaluation_units WHERE id='ud'")
    assert db.execute("SELECT COUNT(*) FROM evaluation_units WHERE id='ud'").fetchone()[0]==0
    assert db.execute("SELECT name FROM schema_migrations WHERE version=8").fetchone()[0].startswith("manage-draft")
    assert db.execute("SELECT name FROM schema_migrations WHERE version=9").fetchone()[0].startswith("persist-pre001")
    assert db.execute("SELECT name FROM schema_migrations WHERE version=10").fetchone()[0].startswith("version-pre001")
    assert db.execute("SELECT version,name FROM schema_migrations WHERE version=6").fetchone()[1].startswith("morfo-dev-002")
    print("MORFO-AVALIA migration PASS: immutable evidence, independent reference, no split leakage")

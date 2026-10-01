"""Crash-safe checkpoints for development benchmarks (standard library only)."""
import hashlib
import json
import os
from pathlib import Path
import time


def file_identity(path):
    path = Path(path).resolve()
    st = path.stat()
    return {'path': str(path), 'bytes': st.st_size, 'mtime_ns': st.st_mtime_ns,
            'ctime_ns': st.st_ctime_ns,
            'device': st.st_dev, 'inode': st.st_ino}


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(8*1024*1024), b''): h.update(chunk)
    return h.hexdigest()


def save(path, value):
    path = Path(path)
    temp = path.with_name(path.name+'.tmp')
    with temp.open('w') as f:
        json.dump(value, f, indent=2); f.write('\n'); f.flush(); os.fsync(f.fileno())
    temp.replace(path)
    fd = os.open(path.parent, os.O_RDONLY)
    try: os.fsync(fd)
    finally: os.close(fd)


def open_run(root, resume, identity):
    """Require unchanged inputs/options/build before reusing completed cases."""
    manifest = root/'run.json'
    if resume:
        if not manifest.exists():
            raise RuntimeError('no resumable run.json; use a new work directory for this older run')
        previous = json.loads(manifest.read_text())
        if previous != identity:
            changed = sorted(k for k in previous.keys() | identity.keys() if previous.get(k) != identity.get(k))
            raise RuntimeError('resume identity changed: '+', '.join(changed))
    else:
        save(manifest, identity)
    result = root/'results.json'
    return json.loads(result.read_text()) if resume and result.exists() else None


def archive_partial(root, paths):
    """Retain interrupted outputs; never reuse their incomplete timings."""
    existing = [Path(p) for p in paths if Path(p).exists() or Path(p).is_symlink()]
    if not existing: return
    archive = root/('interrupted-'+str(time.time_ns()))
    archive.mkdir()
    for path in existing:
        if path.parent != root: raise RuntimeError('partial artifact outside the run directory')
        path.rename(archive/path.name)

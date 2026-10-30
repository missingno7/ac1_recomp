"""Restore an explicitly selected private seed for paired gameplay runs.

The installation is never a destination. Existing workspace userdata is
archived, not removed; links are rejected before copying or moving anything.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import stat

ROOT = Path(__file__).resolve().parents[1]


def linked(path):
    return path.is_symlink() or bool(getattr(path.lstat(), 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT)


def reject_links(path):
    for item in (path, *path.rglob('*')):
        if linked(item):
            raise ValueError(f'userdata seed/archive must not contain links: {item}')


def restore(seed, archive, workspace=ROOT):
    workspace = Path(workspace).resolve()
    work = workspace/'work'
    seed, archive = Path(seed).absolute(), Path(archive).absolute()
    userdata = work/'userdata'
    for path in (seed, archive):
        if not path.resolve().is_relative_to(work.resolve()) or path.resolve().is_relative_to(userdata.resolve()):
            raise ValueError(f'userdata path must be under private work and outside live userdata: {path}')
        for parent in (path, *path.parents):
            if parent == workspace: break
            if parent.exists() and linked(parent):
                raise ValueError(f'linked userdata ancestor: {parent}')
    if not seed.is_dir(): raise ValueError(f'userdata seed is not a directory: {seed}')
    if archive.resolve().is_relative_to(seed.resolve()): raise ValueError('archive must be outside immutable seed')
    if archive.exists(): raise ValueError(f'userdata archive already exists: {archive}')
    if userdata.is_symlink(): raise ValueError('linked live userdata')
    if userdata.exists() and not userdata.is_dir(): raise ValueError('live userdata is not a directory')
    reject_links(seed)
    if userdata.exists(): reject_links(userdata)
    inventory = [dict(path=p.relative_to(seed).as_posix(), bytes=p.stat().st_size,
                      sha256=hashlib.sha256(p.read_bytes()).hexdigest())
                 for p in sorted(seed.rglob('*')) if p.is_file()]
    if userdata.exists():
        archive.parent.mkdir(parents=True, exist_ok=True)
        userdata.rename(archive)
    shutil.copytree(seed, userdata)
    return inventory


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seed', required=True, type=Path)
    parser.add_argument('--archive', required=True, type=Path)
    parser.add_argument('--manifest', required=True, type=Path)
    args=parser.parse_args()
    inventory=restore(args.seed,args.archive)
    args.manifest.write_text(json.dumps(inventory,indent=2)+'\n',encoding='utf-8')

"""Backup and recover the private source-transfer archive beside a stopped save."""
import argparse
import csv
from hashlib import sha256
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tarfile
import tempfile

ARCHIVE = 'transfer-archives.tar.gz'
MANIFEST = 'transfer-archives-manifest.json'
NAME = re.compile(r'[A-Za-z0-9._-]{1,255}')
SHA = re.compile(r'[a-f0-9]{64}')


def require(value, message):
    if not value:
        raise ValueError(message)


def stream_digest(data):
    value = sha256()
    for chunk in iter(lambda: data.read(1024 * 1024), b''):
        value.update(chunk)
    return value.hexdigest()


def digest(path):
    with path.open('rb') as data:
        return stream_digest(data)


def private_directory(path):
    path.mkdir(mode=0o700)
    if os.name == 'nt':
        identity = subprocess.check_output(['whoami', '/user', '/fo', 'CSV', '/nh'], text=True)
        sid = next(csv.reader(identity.strip().splitlines()))[-1]
        require(re.fullmatch(r'S-1-[0-9-]+', sid), 'Cannot identify Windows backup owner')
        subprocess.run(['icacls', str(path), '/inheritance:r', '/grant:r', '*'+sid+':(OI)(CI)F'],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def inspect_directory(directory):
    try:
        info = directory.lstat()
    except FileNotFoundError:
        return {'present': False, 'files': []}
    require(stat.S_ISDIR(info.st_mode), 'Transfer archive path must be a real directory')
    if os.name != 'nt':
        require(stat.S_IMODE(info.st_mode) == 0o700, 'Transfer archive directory is not private')
    files = []
    for entry in sorted(directory.iterdir(), key=lambda row: row.name):
        info = entry.lstat()
        require(NAME.fullmatch(entry.name) and stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
                'Transfer archive directory contains an unsafe entry')
        if os.name != 'nt':
            require(stat.S_IMODE(info.st_mode) == 0o600, 'Transfer archive file is not private')
        files.append({'name': entry.name, 'bytes': info.st_size, 'sha256': digest(entry)})
    return {'present': True, 'files': files}


def verify_inventory(inventory, expected):
    require(SHA.fullmatch(expected) and digest(inventory) == expected, 'Stopped inventory SHA-256 mismatch')


def receipt(directory, doc):
    return {'inventorySha256': doc['inventorySha256'], 'present': doc['present'], 'files': len(doc['files']),
            'archiveSha256': doc['archiveSha256'], 'manifestSha256': digest(directory / MANIFEST)}


def verify(directory, inventory, expected):
    verify_inventory(inventory, expected)
    doc = json.loads((directory / MANIFEST).read_bytes())
    require(doc['inventorySha256'] == expected, 'Archives belong to a different stopped inventory')
    require(type(doc['present']) is bool and (doc['present'] or not doc['files']), 'Invalid archive presence')
    require(digest(directory / ARCHIVE) == doc['archiveSha256'], 'Compressed transfer archive SHA-256 mismatch')
    actual = []
    with tarfile.open(directory / ARCHIVE, 'r:gz') as packed:
        for member in packed:
            require(member.isfile() and NAME.fullmatch(member.name) and member.name not in ('.', '..')
                    and member.mode == 0o600, 'Unsafe compressed archive entry')
            with packed.extractfile(member) as data:
                actual.append({'name': member.name, 'bytes': member.size,
                               'sha256': stream_digest(data)})
    require(actual == doc['files'] and len({f['name'] for f in actual}) == len(actual),
            'Transfer archive manifest does not match compressed entries')
    return doc


def pack(directory, inventory, expected):
    verify_inventory(inventory, expected)
    source = inventory.with_name('transfer-archives')
    manifest = inspect_directory(source)
    with (directory / ARCHIVE).open('xb') as output:
        os.chmod(output.name, 0o600)
        with tarfile.open(fileobj=output, mode='w:gz') as packed:
            for row in manifest['files']:
                info = tarfile.TarInfo(row['name'])
                info.size = row['bytes']
                info.mode = 0o600
                with (source / info.name).open('rb') as data:
                    packed.addfile(info, data)
        output.flush()
        os.fsync(output.fileno())
    require(inspect_directory(source) == manifest, 'Transfer archive changed during stopped backup')
    doc = {**manifest, 'inventorySha256': expected, 'archiveSha256': digest(directory / ARCHIVE)}
    with (directory / MANIFEST).open('x', encoding='utf8') as output:
        os.chmod(output.name, 0o600)
        json.dump(doc, output, ensure_ascii=False, indent=2)
        output.write('\n')
        output.flush()
        os.fsync(output.fileno())
    verify(directory, inventory, expected)
    sync_directory(directory)
    return doc


def sync_directory(directory):
    if os.name != 'nt':
        fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


def restore(directory, inventory, backup_inventory, expected, quarantine):
    doc = verify(directory, backup_inventory, expected)
    source = inventory.with_name('transfer-archives')
    parent = source.parent.resolve(strict=True)
    require(quarantine.parent.resolve(strict=True) == parent and quarantine != source
            and source.resolve().parent == parent and quarantine.resolve().parent == parent,
            'Invalid rollback quarantine location')
    baseline = {field: doc[field] for field in ('present', 'files')}
    current = inspect_directory(source)
    if current == baseline:
        return {'changed': False, 'quarantined': quarantine.exists()}
    saved_candidate = inspect_directory(quarantine)
    require(not (current['present'] and saved_candidate['present']), 'Conflicting rollback quarantine')
    owner = inventory.stat()
    staging = Path(tempfile.mkdtemp(prefix='.transfer-archives-rollback-', dir=source.parent))
    try:
        if baseline['present']:
            if os.name != 'nt':
                os.chown(staging, owner.st_uid, owner.st_gid)
            with tarfile.open(directory / ARCHIVE, 'r:gz') as packed:
                for member in packed:
                    target = staging / member.name
                    fd = os.open(target, os.O_CREAT | os.O_WRONLY | os.O_EXCL, 0o600)
                    with os.fdopen(fd, 'wb') as output, packed.extractfile(member) as data:
                        shutil.copyfileobj(data, output)
                        if os.name != 'nt':
                            os.fchown(output.fileno(), owner.st_uid, owner.st_gid)
                        output.flush()
                        os.fsync(output.fileno())
            require(inspect_directory(staging) == baseline, 'Restored transfer archive failed verification')
            sync_directory(staging)
        if current['present']:
            source.rename(quarantine)
            sync_directory(source.parent)
        if baseline['present']:
            staging.rename(source)
            sync_directory(source.parent)
        require(inspect_directory(source) == baseline, 'Restored transfer archives differ from stopped backup')
        return {'changed': True, 'quarantined': quarantine.exists()}
    finally:
        if staging.exists():
            require(staging.resolve().parent == parent and staging.name.startswith('.transfer-archives-rollback-'),
                    'Unexpected rollback staging directory')
            shutil.rmtree(staging)


def main():
    cli = argparse.ArgumentParser()
    cli.add_argument('action', choices=('init', 'pack', 'verify', 'restore'))
    cli.add_argument('directory', type=Path)
    cli.add_argument('inventory', type=Path, nargs='?')
    cli.add_argument('sha256', nargs='?')
    cli.add_argument('backup_inventory', type=Path, nargs='?')
    cli.add_argument('quarantine', type=Path, nargs='?')
    args = cli.parse_args()
    if args.action == 'init':
        private_directory(args.directory)
    elif args.action == 'restore':
        print(json.dumps(restore(args.directory, args.inventory, args.backup_inventory, args.sha256, args.quarantine)))
    else:
        action = pack if args.action == 'pack' else verify
        print(json.dumps(receipt(args.directory, action(args.directory, args.inventory, args.sha256))))


if __name__ == '__main__':
    main()

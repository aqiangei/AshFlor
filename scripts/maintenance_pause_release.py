"""Validate sealed, isolated daily bonus resume evidence and build per-realm env."""
from hashlib import file_digest
import json
from pathlib import Path, PurePosixPath
import re
import tarfile

PROOF = 'maintenance-pause-proof.json'
FILES = frozenset({'server.wasm', 'server.js', 'deploy-runtime.cjs'})
REQUIRED_INPUTS = frozenset({
    PROOF,
    'maintenance_pause_release.py',
    'ops/scripts/deploy-with-backup.cjs',
    'ops/scripts/stage-drop-bonus-pause.cjs',
})
FLAVORS = {'first': 'one', 'second': 'two-three', 'third': 'two-three'}
OUTCOMES = [
    {'sameDay': True, 'multiplier': 2, 'remaining': 9_000_000},
    {'sameDay': True, 'multiplier': 3, 'remaining': 12_500_000},
    {'sameDay': False, 'multiplier': 1, 'remaining': 0},
]


def require(condition, description):
    if not condition:
        raise ValueError(description)


def sha256(value):
    require(isinstance(value, str) and re.fullmatch(r'[a-f0-9]{64}', value), 'Invalid SHA-256')
    return value


def digest(path):
    with Path(path).open('rb') as f:
        return file_digest(f, 'sha256').hexdigest()


def archive_server_files(path):
    checked = {}
    with tarfile.open(path, mode='r:gz') as archive:
        for member in archive:
            name = PurePosixPath(member.name)
            if name.name not in FILES:
                continue
            require(name.parts == (name.name,) and member.isfile() and name.name not in checked,
                    'Invalid or duplicate archived server artifact')
            with archive.extractfile(member) as f:
                checked[name.name] = file_digest(f, 'sha256').hexdigest()
    require(set(checked) == FILES, 'Missing archived server artifact')
    return checked


def check_receipt(receipt, identity):
    require(isinstance(receipt, dict)
            and (receipt.get('protocol'), receipt.get('contentHash')) == identity
            and receipt.get('outcomes') == OUTCOMES, 'Unexpected bonus resume results')
    files = receipt.get('files', {})
    require(isinstance(files, dict) and set(files) == FILES, 'Missing runtime evidence digest')
    return {name: sha256(value) for name, value in files.items()}


def verify_pause_release(lab, release, rollback_identity):
    lab = Path(lab)
    inputs = release.get('inputs', {})
    require(REQUIRED_INPUTS <= inputs.keys(), 'Maintenance pause support files are missing from the seal')
    for filename in REQUIRED_INPUTS:
        require(digest(lab / filename) == sha256(inputs[filename]), f'Unsealed maintenance input {filename}')
    proof = json.loads((lab / PROOF).read_bytes())
    require(proof.get('schema') == 1, 'Unknown maintenance pause proof schema')
    flavors = set(FLAVORS.values())
    require(set(proof.get('rollback', {})) == set(proof.get('candidate', {})) == flavors,
            'Both release artifact variants require independent evidence')
    candidate_identity = release['protocol'], release['contentHash']
    confirmed = {}
    for flavor in flavors:
        previous = check_receipt(proof['rollback'][flavor], rollback_identity)
        candidate = check_receipt(proof['candidate'][flavor], candidate_identity)
        require(candidate == archive_server_files(lab / f'game-package-{flavor}.tar.gz'),
                f'Tested artifact differs from sealed {flavor} release archive')
        confirmed[flavor] = json.dumps({'rollback': previous, 'candidate': candidate}, separators=(',', ':'))
    return {role: confirmed[flavor] for role, flavor in FLAVORS.items()}

"""Queue confirmed manual transfers for archive and source-account removal.

Inputs are the three latest stopped saves and their independently verified
SHA-256 digests. The tool only writes separate candidate files and a summary.
"""

import argparse
import hashlib
import json
from pathlib import Path

REALM_IDS = {"first": "1", "second": "2", "third": "3"}
REALMS = set(REALM_IDS)


def read_json(path):
    return json.loads(Path(path).read_bytes())


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def user_index(save):
    users = save["users"].values()
    result = {user["id"]: user for user in users}
    if len(result) != len(save["users"]):
        raise ValueError("Duplicate account IDs in save")
    return result


def verify_identity(ref, saves, indexes):
    realm = ref["realm"]
    user = indexes[realm].get(ref["id"])
    if not user or user["username"] != ref["name"] or user.get("admin"):
        raise ValueError("Migration record does not match an ordinary account by ID and name")
    if ref["id"] not in saves[realm]["players"]:
        raise ValueError("Expected character is missing")
    return user


def prepare(saves, manifest):
    if set(saves) != REALMS or manifest["version"] != 1:
        raise ValueError("Expected three standalone databases and a version 1 manifest")

    indexes = {realm: user_index(save) for realm, save in saves.items()}
    archived = {realm: {} for realm in REALMS}
    receipts = {realm: {} for realm in REALMS}
    removed = 0
    targets = set()

    # Validate every recorded account before changing any candidate.
    for move in manifest["moves"]:
        source, target = move["source"], move["target"]
        source_realm, target_realm = source["realm"], target["realm"]
        if source_realm == target_realm:
            raise ValueError("A migration must cross realms")
        target_key = (target_realm, target["id"])
        if target_key in targets:
            raise ValueError("Duplicate migration target")
        targets.add(target_key)
        verify_identity(target, saves, indexes)

        account_id = source["id"]
        if move["sourceState"] == "removed":
            if (account_id in indexes[source_realm] or
                account_id in saves[source_realm]["players"] or
                any(row.get("userId") == account_id for row in saves[source_realm].get("sessions", {}).values())):
                raise ValueError("A previously removed source has reappeared")
            removed += 1
            continue
        if move["sourceState"] != "archived" or account_id in archived[source_realm]:
            raise ValueError("Invalid or duplicate migration source")
        operation = sha256(
            json.dumps([source_realm, account_id, target_realm, target["id"]]).encode()
        )
        existing_receipt = saves[source_realm].get("accountTransferOutgoing", {}).get(operation)
        receipt = {"user": account_id, "state": "transferred", "realm": REALM_IDS[target_realm],
                   "username": target["name"], "delivered": True, "expiresAt": 0}
        if existing_receipt and existing_receipt.get("sourceDeleted"):
            digest = existing_receipt.get("archiveSha256", "")
            if (len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest) or
                    any(existing_receipt.get(k) != v for k, v in receipt.items()) or
                    account_id in indexes[source_realm] or account_id in saves[source_realm]["players"]):
                raise ValueError("Previously archived source has conflicting state")
            removed += 1
            continue
        verify_identity(source, saves, indexes)
        existing = saves[source_realm].get("accountTransferUsers", {}).get(account_id)
        lock = {"state": "transferred", "operation": operation}
        old_lock = dict(lock, operation="manual:" + operation)
        if existing not in (None, lock, old_lock) or existing_receipt not in (None, receipt):
            raise ValueError("Archived source has conflicting transfer state")
        archived[source_realm][account_id] = lock
        receipts[source_realm][operation] = receipt

    summary = {"verifiedTargets": len(targets), "alreadyRemovedSources": removed, "servers": {}}
    for realm in sorted(REALMS):
        locks = archived[realm]
        save = saves[realm]
        removed_sessions = [token for token, row in save.get("sessions", {}).items()
                            if row.get("userId") in locks]
        if locks:
            save.setdefault("accountTransferUsers", {}).update(locks)
            save.setdefault("accountTransferOutgoing", {}).update(receipts[realm])
        for token in removed_sessions:
            del save["sessions"][token]
        summary["servers"][realm] = {
            "sourceLocks": len(locks),
            "revokedSourceSessions": len(removed_sessions),
        }
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--backups", required=True, type=Path,
                        help="JSON mapping realm to an absolute path and verified stopped SHA-256")
    parser.add_argument("--out", required=True, type=Path,
                        help="New directory for the three candidates and validation summary")
    args = parser.parse_args()

    inputs = read_json(args.backups)
    if set(inputs) != REALMS:
        raise ValueError("Missing verified backup for one or more realms")
    saves = {}
    hashes = {}
    for realm in sorted(REALMS):
        path = Path(inputs[realm]["path"])
        if not path.is_absolute():
            raise ValueError("Backup path must be absolute")
        data = path.read_bytes()
        hashes[realm] = sha256(data)
        if hashes[realm] != inputs[realm]["sha256"]:
            raise ValueError(f"Backup SHA-256 mismatch for {realm}")
        saves[realm] = json.loads(data)

    summary = prepare(saves, read_json(args.manifest))
    summary["inputSha256"] = hashes
    summary["outputSha256"] = {}
    args.out.mkdir(mode=0o700)
    for realm, save in saves.items():
        data = json.dumps(save, ensure_ascii=False, separators=(",", ":")).encode()
        with (args.out / f"inventory-{realm}.json").open("xb") as candidate:
            candidate.write(data)
        summary["outputSha256"][realm] = sha256(data)
    (args.out / "verification.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary))


if __name__ == "__main__":
    main()

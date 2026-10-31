# M2 Upload MVP Implementation Plan

**Goal:** Complete the existing M2 file-upload acceptance criteria without reinitializing or replacing Reactor.

**Architecture:** Portable strict commit/error messages; Linux UploadStorage owns private session data and durable receipts; UploadHandler submits one outstanding request per connection through bounded DiskExecutor and returns results to its loop. Linux CLI streams a stable regular source and checks all correlated responses. GUI prototype remains optional; M3 recovery/query is excluded.

**Tech Stack:** C++17, existing epoll Reactor, OpenSSL EVP, Linux openat2/O_TMPFILE/renameat2, CMake/CTest/Python.

**Spec:** AGENTS.md, CODEX_DEVELOPMENT_GUIDE.md M2, existing protocol contracts.

## Global constraints and rulings

- Preserve existing dirty files and tests; no automatic git commit/push.
- No disk/hash/persistence operations on EventLoop; pure protocol remains Qt/POSIX/OpenSSL independent.
- One in-flight disk task per connection; bounded worker capacity and connection limits; maximum file 64MiB, at most 65536 blocks, 32 retained sessions and 512MiB reserved payload per root for this MVP.
- M2 ACK uses synced data plus synced append-only receipt in a named, private session directory. This is the minimal persistent receipt required by ACK semantics, not SQLite migrations, query/resume or crash recovery. Incomplete receipts remain bounded and retained on disconnect; no false resume promise.
- Publish beneath private root/files with no symlinks and no overwrite, same-filesystem rename, fsync directory. Explicit commit result alone reports completed file.
- Qt prototype was initially deferred; after core acceptance passed, an installed Qt6.8.3 matching Kit was verified and the optional connection-only prototype was included. It must not advertise file-upload GUI capability.

## Tasks

- [x] Strict COMMIT_UPLOAD/COMMIT_RESULT/business ERROR and streaming Linux CLI upload; protocol initial red was missing-header compile failure, not claimed as behavior-red.
- [x] UploadStorage: session allocation, actual block SHA256, complete bounded pwrite, fdatasync, receipts, whole-file hash and safe no-replace publication; storage tests.
- [x] UploadHandler: HELLO through commit, weak connection lifetime, bounded DiskExecutor submission/completion, state and request association, integration red/green tests.
- [x] Minimal application read suspension in TcpConnection for async worker backpressure; preserve existing output backpressure.
- [x] Integrate CMake/apps; run Ubuntu Debug/ASan/Release and portable Windows checks, fix failures and independent review findings.
- [x] Build/test optional Qt connection prototype with the existing Windows Kit; 8/8 tests, screenshot checked, no GUI file-upload claim.
- [x] Update progress, use/debug/test guide and milestone evidence; sync VM; no push.

## Review focus

- Disk failure before ACK must never produce durable confirmation.
- Disconnect while a worker runs must not access dead connection or discard acknowledged data.
- Existing destinations, parent symlinks and concurrent same-target requests must not overwrite data.
- Empty files, short last blocks, repeated chunks/commit and mutated source must be handled honestly.
- Queue/connection/session/file limits and timeouts must prevent unbounded growth.

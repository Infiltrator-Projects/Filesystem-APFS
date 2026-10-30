# APFS migration

The original APFS parser, checkpoint/spaceman/catalog engine, writable Defrag/Recover worker and deterministic Test Media formatter and verifier now compile into the independent APFS.so. The application must use only versioned host service and corpus callbacks, not local filesystem algorithms. Standalone tests and end-to-end Defragmenter integration must pass before qualification. Firmware and KO drivers are separate requirements.

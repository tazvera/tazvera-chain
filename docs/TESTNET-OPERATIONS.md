# Testnet node operations guide

**This guide helps identify and safely handle common problems on a running testnet node.**

**First:** When any problem occurs, record the node screen/logs, time, chain-file path, and last successful height.

**Never manually edit or delete a chain file, or replace it with a file from another machine.**

**Preserving data matters on a testnet too; stop the node cleanly and keep a backup copy of the file before recovery.**

**These steps are for the current prototype; this is not a production recovery procedure.**

## 1. Block production stopped — height is not increasing

**Symptoms**
- Height has remained unchanged for some time.
- No new block appears or producer activity has stopped.

**Possible causes and actions**
- **Producer is stopped or the node is only in sync/listen mode:** Check the process and startup command. If the producer runs separately, check its status and logs. To start a producer, use only a documented command available in your binary; do not guess a new flag.
- **The machine clock is behind:** Check the system time and time zone. Correct the clock (see issue 2 below), then check node/producer logs.
- **Disk is full:** Check free space on the drive containing the chain file. Safely free space; do not delete the chain file. Then stop and restart the node cleanly and check the logs for write errors.
- **Another node is ahead or network sync has stopped:** Check the peer connection, peer logs, and the heights/hashes of both nodes. If a fork is suspected, follow issue 4.

An unchanged height by itself does not prove that the chain is broken. First check the producer, clock, disk, and peer status.

## 2. Warning such as “timestamp beyond this clock”

**Symptom:** The node reports that a block timestamp is ahead of the machine's current clock and refuses to accept it or prints a warning.

**Cause:** The system clocks on two machines may differ—one machine may be fast, the other behind, or the time-zone/NTP configuration may be wrong.

**Actions**
1. Check the date, time, and time zone on both machines. Changing only the time zone does not necessarily correct UTC time—check the actual clock too.
2. Enable automatic time synchronization on both machines (Windows: Settings → Time & language → Date & time → Set time automatically → Sync now; Linux: check your system's NTP/chrony service).
3. Check the logs again after some time. If the warning remains, record both machines' UTC times and the block timestamp; do not try to remove the warning by manually changing a block or chain file.
4. Correct the clock on the affected node, then resume normal sync/restart. Do not manually rewrite an old block.

## 3. Node exited with code 3 — chain file could not be written

**Symptom:** The process stops and the terminal may show `cannot write`, `cannot load`, or a file-related error.

**Important:** Exit code 3 does not indicate only a disk-write failure. During current node startup, chain-load/refusal errors can also return code 3. Read the complete terminal error message.

**Possible causes**
- Disk or volume is full.
- The node does not have write permission in that folder.
- The file is locked by another process, backup tool, or sync tool.
- The path is wrong, the drive is unmounted, or the filesystem has become read-only.
- The chain file is unreadable/corrupt or does not match the trusted genesis pin.

**Actions**
1. Record the complete error message and exact chain-file path.
2. Check free disk space, folder permissions, mounted drive, and file locks. If a backup/sync tool has the file open, let it finish or stop it safely.
3. Stop the node cleanly. **Do not** open the chain file in a text editor to repair it, truncate it, delete it, or overwrite it with another file.
4. After fixing storage/permission problems, keep a backup copy and restart the node with the same trusted chain file and genesis pin.
5. If the error is “wrong chain/genesis” or a load/parse failure, confirm the correct trusted pin/path instead of replacing the file. If in doubt, stop further writes and give the logs to the maintainer.

## 4. Two nodes are producing blocks on the same chain — fork race

**Symptoms:** The two nodes have different heights, or the same height but different latest block hashes. Their logs may show different blocks being produced at around the same time.

**Cause:** Two producers are trying to create blocks from the same chain state. This race can create separate valid-looking branches; do not choose the correct branch by looking only at the highest height.

**Actions**
1. Stop both producers; do not delete the chain files or copy one over the other.
2. Preserve both nodes' heights, latest block hashes, genesis commitment, and logs. Identify the block where divergence began.
3. Check network/peer sync status. To choose a branch, use only a documented fork-selection/recovery procedure or confirmation from the maintainer.
4. Until the canonical branch is clear, keep separate backups of both files and do not produce new blocks on either node.
5. If no method is available to return to the selected branch, do not edit chain files manually; send both hashes and logs to the maintainer.

## 5. Opening an old chain file with a new binary

**Symptoms:** The new binary refuses to load the chain file, reports a version/format/parse error, or the trusted genesis commitment does not match.

**Cause:** The protocol version or persisted file format may have changed. A new binary is not automatically guaranteed to read old data. A genesis-pin mismatch may also mean that the wrong chain file was selected.

**Actions**
1. Keep a separate backup copy of the old chain file; do not attempt migration on the original.
2. Check release notes and migration instructions: does the new binary support the old format, and is an explicit migration required?
3. Use the same trusted genesis commitment that was issued for that chain. Do not bypass a pin mismatch.
4. If no compatible format or migration tool is available, open a backup copy with the old binary and ask the maintainer for migration instructions. Do not repeatedly try to write a file created by the old binary using the new binary.
5. Changing only the protocol version does not change the chain identity/genesis; however, file-format/validation rules may change. Compatibility must be clear before upgrading.

## 6. Stop a node correctly

**Correct method — Ctrl+C**
1. Go to the terminal where the node is running.
2. Press `Ctrl+C` once and let the process stop.
3. Wait for shutdown/exit to finish in the terminal. Then, if needed, back up the chain file.

**Incorrect methods**
- Killing the process suddenly with `kill -9` or force-kill.
- Powering off the computer or unplugging it while the node is writing.
- Copying/replacing the chain file while the process is running.

After an abrupt stop, the latest change may not have been fully written to disk. This does not mean corruption will happen every time, but recovery is not guaranteed either. If an error appears on restart, do not repair the file manually—keep a backup first and ask for help with the logs.

---

## Quick checklist

- [ ] The correct node/binary and correct chain file are running.
- [ ] The producer is running; the clock is synchronized.
- [ ] There is disk space and the process has the correct file permissions.
- [ ] Both nodes have the same genesis commitment.
- [ ] The latest block hash has been compared along with the height.
- [ ] Shutdown was done with Ctrl+C and a backup of the chain file is safe.

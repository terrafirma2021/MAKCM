#include <cassert>
#include <cstring>

#include "PairOtaTransaction.h"

using namespace makcm::ota;

namespace {

PairBoardFact rolledBackFact(Target role, uint8_t seed) {
    PairBoardFact fact{};
    fact.present = true;
    fact.identity_complete = true;
    fact.role = role;
    fact.local_phase = snapshot::Phase::RolledBack;
    fact.image_state = 2;
    fact.running_slot = 0x10;
    fact.boot_slot = 0x10;
    fact.target_slot = 0x11;
    fact.previous_slot = 0x10;
    fact.last_command = Command::Status;
    fact.last_response_command = Command::StatusResponse;
    fact.completed_outcome = snapshot::Outcome::Aborted;
    fact.completed_terminal_command = Command::Abort;
    fact.transaction_id = 0x12345678;
    fact.target_generation = 15;
    fact.previous_generation = 14;
    fact.partition_schema = role == Target::Left ? 0x4c010001 : 0x52010001;
    fact.image_size = 0x70000;
    fact.record_sequence = 7;
    fact.last_request_sequence = 21 + seed;
    fact.completed_transaction_id = fact.transaction_id;
    fact.completed_terminal_sequence = 21 + seed;
    fact.reset_reason = 1;
    fact.boot_attempt = 1;
    std::memset(fact.image_sha256, seed, 32);
    std::memset(fact.manifest_sha256, seed + 1, 32);
    std::memset(fact.identity_digest, seed + 2, 32);
    std::memset(fact.previous_image_sha256, seed + 3, 32);
    std::memcpy(fact.running_image_sha256, fact.previous_image_sha256, 32);
    std::memcpy(fact.hardware_revision, "makcm-s3-r1", 12);
    std::memcpy(fact.project_name,
                role == Target::Left ? "makcm_left_idf" : "makcm_right_arduino",
                role == Target::Left ? 15 : 20);
    std::memcpy(fact.app_version, "makcm-g15", 10);
    return fact;
}

} // namespace

int main() {
    PairTransactionRecord record{};
    record.record_sequence = 9;
    record.stage = PairStage::RolledBack;
    record.transaction_id = 0x12345678;
    record.target_generation = 15;
    record.left = rolledBackFact(Target::Left, 0x11);
    record.right = rolledBackFact(Target::Right, 0x21);
    record.completed.present = true;
    record.completed.outcome = snapshot::Outcome::RolledBack;
    record.completed.terminal_command = Command::Abort;
    record.completed.transaction_id = record.transaction_id;
    record.completed.target_generation = record.target_generation;
    record.completed.terminal_sequence = 22;
    record.completed.record_sequence = record.record_sequence;
    record.completed.left_target_slot = record.left.target_slot;
    record.completed.right_target_slot = record.right.target_slot;
    std::memcpy(record.completed.left_image_sha256,
                record.left.image_sha256, 32);
    std::memcpy(record.completed.right_image_sha256,
                record.right.image_sha256, 32);
    std::memcpy(record.completed.left_manifest_sha256,
                record.left.manifest_sha256, 32);
    std::memcpy(record.completed.right_manifest_sha256,
                record.right.manifest_sha256, 32);
    std::memcpy(record.completed.left_identity_digest,
                record.left.identity_digest, 32);
    std::memcpy(record.completed.right_identity_digest,
                record.right.identity_digest, 32);
    sealPairRecord(record);

    assert(validPairBoardFact(record.left, Target::Left));
    assert(validPairBoardFact(record.right, Target::Right));
    assert(validPairRecord(record));
    assert(pairRecordHasExactPairRollback(record));
    assert(!pairRecordHasExactPairCommit(record));
    assert(!pairRecordRequiresHostReconcile(record));

    record.right.running_slot = record.right.target_slot;
    sealPairRecord(record);
    assert(!validPairRecord(record));
    assert(!pairRecordHasExactPairRollback(record));
    return 0;
}

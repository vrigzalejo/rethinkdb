// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "clustering/table_manager/table_metadata.hpp"

RDB_IMPL_SERIALIZABLE_2_SINCE_v2_1(
    multi_table_manager_bcard_t::timestamp_t::epoch_t, timestamp, id);
RDB_IMPL_SERIALIZABLE_2_SINCE_v2_1(
    multi_table_manager_bcard_t::timestamp_t, epoch, log_index);
RDB_IMPL_SERIALIZABLE_3_FOR_CLUSTER(
    multi_table_manager_bcard_t,
    action_mailbox, get_config_mailbox, server_id);
RDB_IMPL_SERIALIZABLE_3_FOR_CLUSTER(
    table_manager_bcard_t::leader_bcard_t,
    uuid, set_config_mailbox, contract_ack_minidir_bcard);
RDB_IMPL_SERIALIZABLE_7_FOR_CLUSTER(
    table_manager_bcard_t,
    leader, timestamp, raft_member_id, raft_business_card,
    execution_bcard_minidir_bcard, get_status_mailbox, server_id);
RDB_IMPL_SERIALIZABLE_3_FOR_CLUSTER(
    table_server_status_t, timestamp, state, contract_acks);

RDB_IMPL_SERIALIZABLE_3_SINCE_v2_1(table_persistent_state_t::active_t,
    epoch, raft_member_id, raft_state);
RDB_IMPL_SERIALIZABLE_2_SINCE_v2_1(table_persistent_state_t::inactive_t,
    second_hand_config, timestamp);
RDB_IMPL_SERIALIZABLE_1_SINCE_v2_1(table_persistent_state_t, value);

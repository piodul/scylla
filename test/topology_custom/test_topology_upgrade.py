#
# Copyright (C) 2024-present ScyllaDB
#
# SPDX-License-Identifier: AGPL-3.0-or-later
#

import asyncio
import functools
import logging
import operator
import pytest
import time

from cassandra.cluster import Session # type: ignore # pylint: disable=no-name-in-module
from cassandra.pool import Host # type: ignore # pylint: disable=no-name-in-module

from test.pylib.manager_client import ManagerClient
from test.pylib.util import wait_for_cql_and_get_hosts, wait_for
from test.topology.util import reconnect_driver, restart, enter_recovery_state, \
        delete_raft_data_and_upgrade_state, log_run_time, wait_until_upgrade_finishes as wait_until_schema_upgrade_finishes

async def wait_until_topology_upgrade_finishes(manager: ManagerClient, ip_addr: str, deadline: float):
    async def check():
        status = await manager.api.raft_topology_upgrade_status(ip_addr)
        return status == "done" or None
    await wait_for(check, deadline=deadline, period=1.0)


async def check_system_topology_and_cdc_generations_v3_consistency(manager: ManagerClient, host: Host):
    topo_res = await manager.cql.run_async("SELECT * FROM system.topology", host=host)
    assert len(topo_res) != 0

    logging.info("Dumping the state of system.topology:")
    for row in topo_res:
        logging.info(f"  {row}")

    for row in topo_res:
        assert row.host_id is not None
        assert row.datacenter is not None
        assert row.ignore_msb is not None
        assert row.node_state == "normal"
        assert row.num_tokens is not None
        assert row.rack is not None
        assert row.release_version is not None
        assert row.supported_features is not None
        assert row.shard_count is not None
        assert row.tokens is not None

        assert len(row.tokens) == row.num_tokens

    assert topo_res[0].current_cdc_generation_timestamp is not None
    assert topo_res[0].current_cdc_generation_uuid is not None
    assert topo_res[0].fence_version is not None
    assert topo_res[0].upgrade_state == "done"

    assert host.host_id in (row.host_id for row in topo_res)

    computed_enabled_features = functools.reduce(operator.and_, (frozenset(row.supported_features) for row in topo_res))
    assert topo_res[0] is not None
    enabled_features = frozenset(topo_res[0].enabled_features)
    assert enabled_features == computed_enabled_features
    assert "SUPPORTS_CONSISTENT_TOPOLOGY_CHANGES" in enabled_features

    cdc_res = await manager.cql.run_async("SELECT * FROM system.cdc_generations_v3", host=host)
    assert len(cdc_res) != 0

    all_generations = frozenset(row.id for row in cdc_res)
    assert topo_res[0].current_cdc_generation_uuid in all_generations

@pytest.mark.asyncio
@log_run_time
async def test_topology_upgrade_basic(request, manager: ManagerClient):
    # First, create a cluster in legacy mode
    cfg = {'enable_user_defined_functions': False,
           'experimental_features': ['consistent-topology-changes'],
           'error_injections_at_startup': ['force_gossip_based_join']}
    
    servers = [await manager.server_add(config=cfg)]
    # Disable injections for the subsequent nodes
    del cfg['error_injections_at_startup']

    servers += [await manager.server_add(config=cfg) for _ in range(2)]
    cql = manager.cql
    assert(cql)

    logging.info("Waiting until driver connects to every server")
    hosts = await wait_for_cql_and_get_hosts(cql, servers, time.time() + 60)

    logging.info(f"Restarting hosts {hosts} with experimental topology on raft enabled")
    # TODO: Apparently, entering recovery state doesn't work - fix it
    # await asyncio.gather(*(enter_recovery_state(cql, h) for h in hosts))
    await asyncio.gather(*(restart(manager, srv) for srv in servers))
    cql = await reconnect_driver(manager)

    logging.info("Cluster restarted, waiting until driver reconnects to every server")
    hosts = await wait_for_cql_and_get_hosts(cql, servers, time.time() + 60)
    logging.info(f"Driver reconnected, hosts: {hosts}")

    logging.info("Checking the upgrade state on all nodes")
    for host in hosts:
        status = await manager.api.raft_topology_upgrade_status(host.address)
        assert status == "not_upgraded"

    logging.info("Waiting until all nodes see others as alive")
    await asyncio.gather(*(manager.server_sees_others(srv.server_id, len(servers) - 1) for srv in servers))

    logging.info("Triggering upgrade to raft topology")
    await manager.api.upgrade_to_raft_topology(hosts[0].address)

    # TODO: Check that trying to trigger this again fails

    logging.info("Waiting until upgrade finishes")
    await asyncio.gather()

    logging.info(f"Cluster restarted, waiting until driver reconnects to every server")
    hosts = await wait_for_cql_and_get_hosts(cql, servers, time.time() + 60)

    logging.info(f"Driver reconnected, hosts: {hosts}. Waiting until upgrade finishes")
    await asyncio.gather(*(wait_until_topology_upgrade_finishes(manager, h.address, time.time() + 60) for h in hosts))

    logging.info("Checking consistency of data in system.topology and system.cdc_generations_v3")
    await asyncio.gather(*(check_system_topology_and_cdc_generations_v3_consistency(manager, h) for h in hosts))

    logging.info("Booting new node")
    await manager.server_add(config=cfg)

    logging.info("Checking consistency of data in system.topology and system.cdc_generations_v3")
    await asyncio.gather(*(check_system_topology_and_cdc_generations_v3_consistency(manager, h) for h in hosts))


async def delete_raft_topology_state(cql: Session, host: Host):
    await cql.run_async("truncate table system.topology", host=host)


@pytest.mark.asyncio
@log_run_time
async def test_topology_recovery_basic(request, manager: ManagerClient):
    # First, create a cluster in legacy mode
    cfg = {'enable_user_defined_functions': False,
           'experimental_features': ['consistent-topology-changes']}
    servers = [await manager.server_add(config=cfg) for _ in range(3)]
    cql = manager.cql
    assert(cql)

    logging.info("Waiting until driver connects to every server")
    hosts = await wait_for_cql_and_get_hosts(cql, servers, time.time() + 60)

    logging.info(f"Restarting hosts {hosts} in recovery mode")
    await asyncio.gather(*(enter_recovery_state(cql, h) for h in hosts))
    await asyncio.gather(*(restart(manager, srv) for srv in servers))
    cql = await reconnect_driver(manager)

    logging.info("Cluster restarted, waiting until driver reconnects to every server")
    hosts = await wait_for_cql_and_get_hosts(cql, servers, time.time() + 60)
    logging.info(f"Driver reconnected, hosts: {hosts}")

    logging.info("Waiting until all nodes see others as alive")
    await asyncio.gather(*(manager.server_sees_others(srv.server_id, len(servers) - 1, time.time() + 60) for srv in servers))

    logging.info(f"Deleting Raft data and upgrade state on {hosts}")
    await asyncio.gather(*(delete_raft_topology_state(cql, h) for h in hosts))
    await asyncio.gather(*(delete_raft_data_and_upgrade_state(cql, h) for h in hosts))

    logging.info(f"Restarting hosts {hosts}")
    await asyncio.gather(*(restart(manager, srv) for srv in servers))
    cql = await reconnect_driver(manager)

    logging.info("Cluster restarted, waiting until driver reconnects to every server")
    hosts = await wait_for_cql_and_get_hosts(cql, servers, time.time() + 60)
    logging.info(f"Driver reconnected, hosts: {hosts}")

    logging.info("Waiting until all nodes see others as alive")
    await asyncio.gather(*(manager.server_sees_others(srv.server_id, len(servers) - 1, time.time() + 60) for srv in servers))

    logging.info(f"Driver reconnected, hosts: {hosts}. Waiting until upgrade to raft schema finishes")
    await asyncio.gather(*(wait_until_schema_upgrade_finishes(cql, h, time.time() + 60) for h in hosts))

    logging.info("Checking the topology upgrade state on all nodes")
    for host in hosts:
        status = await manager.api.raft_topology_upgrade_status(host.address)
        assert status == "not_upgraded"

    logging.info("Triggering upgrade to raft topology")
    await manager.api.upgrade_to_raft_topology(hosts[0].address)

    logging.info(f"Driver reconnected, hosts: {hosts}. Waiting until upgrade finishes")
    await asyncio.gather(*(wait_until_topology_upgrade_finishes(manager, h.address, time.time() + 60) for h in hosts))

    logging.info("Checking consistency of data in system.topology and system.cdc_generations_v3")
    await asyncio.gather(*(check_system_topology_and_cdc_generations_v3_consistency(manager, h) for h in hosts))

    logging.info("Booting new node")
    servers += [await manager.server_add(config=cfg)]
    hosts = await wait_for_cql_and_get_hosts(cql, servers, time.time() + 60)

    logging.info("Checking consistency of data in system.topology and system.cdc_generations_v3")
    await asyncio.gather(*(check_system_topology_and_cdc_generations_v3_consistency(manager, h) for h in hosts))


@pytest.mark.asyncio
@log_run_time
async def test_topology_recovery_after_majority_loss(request, manager: ManagerClient):
    cfg = {'enable_user_defined_functions': False,
           'experimental_features': ['consistent-topology-changes']}
    servers = [await manager.server_add(config=cfg) for _ in range(3)]
    cql = manager.cql
    assert(cql)

    logging.info("Waiting until driver connects to every server")
    hosts = await wait_for_cql_and_get_hosts(cql, servers, time.time() + 60)

    srv1, *others = servers

    logging.info(f"Killing all nodes except {srv1}")
    await asyncio.gather(*(manager.server_stop_gracefully(srv.server_id) for srv in others))

    logging.info(f"Entering recovery state on {srv1}")
    host1 = next(h for h in hosts if h.address == srv1.ip_addr)
    await enter_recovery_state(cql, host1)
    await restart(manager, srv1)
    cql = await reconnect_driver(manager)

    logging.info("Node restarted, waiting until driver connects")
    host1 = (await wait_for_cql_and_get_hosts(cql, [srv1], time.time() + 60))[0]

    for i in range(len(others)):
        to_remove = others[i]
        ignore_dead_ips = [srv.ip_addr for srv in others[i+1:]]
        logging.info(f"Removing {to_remove} using {srv1} with ignore_dead: {ignore_dead_ips}")
        await manager.remove_node(srv1.server_id, to_remove.server_id, ignore_dead_ips)

    logging.info(f"Deleting old Raft data and upgrade state on {host1} and restarting")
    await delete_raft_topology_state(cql, host1)
    await delete_raft_data_and_upgrade_state(cql, host1)
    await restart(manager, srv1)
    cql = await reconnect_driver(manager)

    logging.info("Node restarted, waiting until driver connects")
    host1 = (await wait_for_cql_and_get_hosts(cql, [srv1], time.time() + 60))[0]

    logging.info(f"Driver reconnected, host: {host1}. Waiting until upgrade to raft schema finishes.")
    await wait_until_schema_upgrade_finishes(cql, host1, time.time() + 60)

    logging.info("Triggering upgrade to raft topology")
    await manager.api.upgrade_to_raft_topology(host1.address)

    logging.info(f"Waiting until upgrade to raft topology finishes.")
    await wait_until_topology_upgrade_finishes(manager, host1.address, time.time() + 60)

    logging.info("Checking consistency of data in system.topology and system.cdc_generations_v3")
    await check_system_topology_and_cdc_generations_v3_consistency(manager, host1)

    logging.info(f"Add two more nodes")
    servers = [srv1] + await manager.servers_add(2, config=cfg)
    hosts = await wait_for_cql_and_get_hosts(cql, servers, time.time() + 60)

    logging.info("Checking consistency of data in system.topology and system.cdc_generations_v3")
    await asyncio.gather(*(check_system_topology_and_cdc_generations_v3_consistency(manager, h) for h in hosts))

#
# Copyright (C) 2025-present ScyllaDB
#
# SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
#
from test.pylib.manager_client import ManagerClient

import asyncio
import pytest
import time
import logging

from test.cluster.conftest import skip_mode
from test.pylib.util import wait_for_view, wait_for_first_completed, gather_safely
from test.pylib.internal_types import ServerInfo, HostID
from test.pylib.tablets import get_tablet_replicas
from test.cluster.mv.tablets.test_mv_tablets import pin_the_only_tablet
from test.cluster.util import new_test_keyspace

from cassandra.cluster import Session, ConsistencyLevel, EXEC_PROFILE_DEFAULT # type: ignore
from cassandra.cqltypes import Int32Type # type: ignore
from cassandra.policies import FallthroughRetryPolicy # type: ignore
from cassandra.query import SimpleStatement, BoundStatement # type: ignore

logger = logging.getLogger(__name__)


ROW_COUNT = 1000
VIEW_BUILDING_WORKER_PAUSE_BUILD_RANGE_TASK = "view_building_worker_pause_build_range_task"
VIEW_BUILDING_WORKER_PAUSE_STAGING_GENERATION_TASK = "?"
VIEW_BUILDING_COORDINATOR_PAUSE_MAIN_LOOP = "view_building_coordinator_pause_main_loop"


async def pause_view_build_coordinator(manager: ManagerClient):
    """Pause view build coordinator."""
    servers = await manager.running_servers()
    await asyncio.gather(*(manager.api.enable_injection(s.ip_addr, VIEW_BUILDING_COORDINATOR_PAUSE_MAIN_LOOP, one_shot=True) for s in servers))


async def unpause_view_build_coordinator(manager: ManagerClient):
    """Unpause the view build coordinator."""
    servers = await manager.running_servers()
    await asyncio.gather(*(manager.api.message_injection(s.ip_addr, VIEW_BUILDING_COORDINATOR_PAUSE_MAIN_LOOP) for s in servers))
    await asyncio.gather(*(manager.api.disable_injection(s.ip_addr, VIEW_BUILDING_COORDINATOR_PAUSE_MAIN_LOOP) for s in servers))


async def pause_view_building_tasks(manager: ManagerClient, token: int | None = None):
    servers = await manager.running_servers()
    params = {}
    if token is not None:
        params["token"] = token
    await asyncio.gather(*(manager.api.enable_injection(s.ip_addr, VIEW_BUILDING_WORKER_PAUSE_BUILD_RANGE_TASK, one_shot=True, parameters=params) for s in servers))


async def unpause_view_building_tasks(manager: ManagerClient):
    servers = await manager.running_servers()
    await asyncio.gather(*(manager.api.message_injection(s.ip_addr, VIEW_BUILDING_WORKER_PAUSE_BUILD_RANGE_TASK) for s in servers))
    await asyncio.gather(*(manager.api.disable_injection(s.ip_addr, VIEW_BUILDING_WORKER_PAUSE_BUILD_RANGE_TASK) for s in servers))


async def disable_tablet_load_balancing_on_all_servers(manager: ManagerClient):
    servers = await manager.running_servers()
    await asyncio.gather(*(manager.api.disable_tablet_balancing(s.ip_addr) for s in servers))


async def mark_all_servers(manager: ManagerClient) -> list[int]:
    servers = await manager.running_servers()
    logs = await asyncio.gather(*(manager.server_open_log(s.server_id) for s in servers))
    return await asyncio.gather(*(l.mark() for l in logs))


async def wait_for_message_on_any_server(manager: ManagerClient, message: str, marks: list[int]):
    servers = await manager.running_servers()
    logs = await asyncio.gather(*(manager.server_open_log(s.server_id) for s in servers))
    assert len(servers) == len(marks)
    await wait_for_first_completed([l.wait_for(message, from_mark=m, timeout=60) for l, m in zip(logs, marks)])


async def wait_for_some_view_build_tasks_to_get_stuck(manager: ManagerClient, marks: list[int]):
    return await wait_for_message_on_any_server(manager, "view_build_worker: paused, waiting for message", marks)


async def wait_for_message_on_all_servers(manager: ManagerClient, message: str, marks: list[int]):
    servers = await manager.running_servers()
    logs = await asyncio.gather(*(manager.server_open_log(s.server_id) for s in servers))
    assert len(servers) == len(marks)
    await gather_safely(*(l.wait_for(message, from_mark=m, timeout=60) for l, m in zip(logs, marks)))


async def populate_base_table(cql: Session, ks: str, tbl: str):
    for i in range(ROW_COUNT):
        await cql.run_async(f"INSERT INTO {ks}.{tbl} (key, c, v) VALUES ({i // 10}, {i % 10}, '{i}')")


async def check_view_contents(cql: Session, ks: str, view: str, partition_list: list[int] | None = None):
    partition_list = partition_list or list(range(ROW_COUNT))
    rows = frozenset(map(tuple, await cql.run_async(f"SELECT c, key, v FROM {ks}.{view}")))
    expected_rows = frozenset((i % 10, i // 10, str(i)) for i in partition_list)
    assert rows == expected_rows


async def get_viable_tablet_migration(manager: ManagerClient, ks: str, tbl: str, token: int) -> tuple[tuple[HostID, int], tuple[HostID, int]]:
    servers = await manager.running_servers()
    replicas = await get_tablet_replicas(manager, servers[0], ks, tbl, token)
    used_hosts = [host_id for host_id, _ in replicas]
    for s in servers:
        candidate_target_replica = await manager.get_host_id(s.server_id)
        if candidate_target_replica not in used_hosts:
            # Reuse the shard, but change the host
            return (replicas[0], (candidate_target_replica, replicas[0][1]))
    
    pytest.fail("Couldn't get a viable target for the tablet migration")


@pytest.mark.skip(reason = "todo")
@pytest.mark.asyncio
async def test_build_one_view(manager: ManagerClient):
    node_count = 4
    servers = await manager.servers_add(node_count)
    cql, _ = await manager.get_ready_cql(servers)
    async with new_test_keyspace(manager, f"WITH replication = {{'class': 'NetworkTopologyStrategy', 'replication_factor': 2}} AND tablets = {{'enabled': true}}") as ks:
        await cql.run_async(f"CREATE TABLE {ks}.tab (key int, c int, v text, PRIMARY KEY (key, c))")
        await populate_base_table(cql, ks, "tab")

        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key IS NOT NULL AND v IS NOT NULL PRIMARY KEY (c, key, v) ")
        await wait_for_view(cql, 'mv_cf_view', node_count)
        await check_view_contents(cql, ks, "mv_cf_view")


# @pytest.mark.skip(reason = "crashes in filtering code - preexisting bug?")
@pytest.mark.asyncio
async def test_build_filtered_view(manager: ManagerClient):
    node_count = 4
    servers = await manager.servers_add(node_count)
    cql, _ = await manager.get_ready_cql(servers)
    async with new_test_keyspace(manager, f"WITH replication = {{'class': 'NetworkTopologyStrategy', 'replication_factor': 2}} AND tablets = {{'enabled': true}}") as ks:
        await cql.run_async(f"CREATE TABLE {ks}.tab (key int, c int, v text, PRIMARY KEY (key, c))")
        await populate_base_table(cql, ks, "tab")

        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key in (1, 4, 9) AND v IS NOT NULL PRIMARY KEY (c, key, v) ")
        await wait_for_view(cql, 'mv_cf_view', node_count)
        await check_view_contents(cql, ks, "mv_cf_view", partition_list=[1, 4, 9])


@pytest.mark.skip(reason = "todo")
@pytest.mark.asyncio
@skip_mode("release", "error injections are not supported in release mode")
async def test_build_two_views(manager: ManagerClient):
    node_count = 4
    servers = await manager.servers_add(node_count)
    cql, _ = await manager.get_ready_cql(servers)
    async with new_test_keyspace(manager, f"WITH replication = {{'class': 'NetworkTopologyStrategy', 'replication_factor': 2}} AND tablets = {{'enabled': true}}") as ks:
        await cql.run_async(f"CREATE TABLE {ks}.tab (key int, c int, v text, PRIMARY KEY (key, c))")
        await populate_base_table(cql, ks, "tab")

        await pause_view_build_coordinator(manager)
        
        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view1 AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key IS NOT NULL AND v IS NOT NULL PRIMARY KEY (c, key, v) ")
        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view2 AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key IS NOT NULL AND v IS NOT NULL PRIMARY KEY (c, key, v) ")
    
        await unpause_view_build_coordinator(manager)

        await wait_for_view(cql, 'mv_cf_view1', node_count)
        await wait_for_view(cql, 'mv_cf_view2', node_count)
        await check_view_contents(cql, ks, "mv_cf_view1")
        await check_view_contents(cql, ks, "mv_cf_view2")


@pytest.mark.skip(reason = "done")
@pytest.mark.asyncio
async def test_add_view_while_build_in_progress(manager: ManagerClient):
    node_count = 4
    servers = await manager.servers_add(node_count)
    cql, _ = await manager.get_ready_cql(servers)
    async with new_test_keyspace(manager, f"WITH replication = {{'class': 'NetworkTopologyStrategy', 'replication_factor': 2}} AND tablets = {{'enabled': true}}") as ks:
        await cql.run_async(f"CREATE TABLE {ks}.tab (key int, c int, v text, PRIMARY KEY (key, c))")
        await populate_base_table(cql, ks, "tab")

        marks = await mark_all_servers(manager)
        await pause_view_building_tasks(manager)

        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view1 AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key IS NOT NULL AND v IS NOT NULL PRIMARY KEY (c, key, v) ")
        
        await wait_for_some_view_build_tasks_to_get_stuck(manager, marks)

        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view2 AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key IS NOT NULL AND v IS NOT NULL PRIMARY KEY (c, key, v) ")

        await unpause_view_building_tasks(manager)

        await wait_for_view(cql, 'mv_cf_view1', node_count)
        await wait_for_view(cql, 'mv_cf_view2', node_count)
        await check_view_contents(cql, ks, "mv_cf_view1")
        await check_view_contents(cql, ks, "mv_cf_view2")


@pytest.mark.skip(reason = "done")
@pytest.mark.asyncio
async def test_remove_all_views_while_build_in_progress(manager: ManagerClient):
    node_count = 4
    servers = await manager.servers_add(node_count)
    cql, _ = await manager.get_ready_cql(servers)
    async with new_test_keyspace(manager, f"WITH replication = {{'class': 'NetworkTopologyStrategy', 'replication_factor': 2}} AND tablets = {{'enabled': true}}") as ks:
        await cql.run_async(f"CREATE TABLE {ks}.tab (key int, c int, v text, PRIMARY KEY (key, c))")
        await populate_base_table(cql, ks, "tab")

        marks = await mark_all_servers(manager)
        await pause_view_building_tasks(manager)

        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key IS NOT NULL AND v IS NOT NULL PRIMARY KEY (c, key, v) ")
        
        await wait_for_some_view_build_tasks_to_get_stuck(manager, marks)

        await cql.run_async(f"DROP MATERIALIZED VIEW {ks}.mv_cf_view")

        await unpause_view_building_tasks(manager)


@pytest.mark.skip(reason = "looks like there is a bug in the implementation - view building coordinator gets stuck on data_dictionary::no_such_column_family")
@pytest.mark.asyncio
async def test_remove_some_view_while_build_in_progress(manager: ManagerClient):
    node_count = 4
    servers = await manager.servers_add(node_count)
    cql, _ = await manager.get_ready_cql(servers)
    async with new_test_keyspace(manager, f"WITH replication = {{'class': 'NetworkTopologyStrategy', 'replication_factor': 2}} AND tablets = {{'enabled': true}}") as ks:
        await cql.run_async(f"CREATE TABLE {ks}.tab (key int, c int, v text, PRIMARY KEY (key, c))")
        await populate_base_table(cql, ks, "tab")

        marks = await mark_all_servers(manager)
        await pause_view_building_tasks(manager)

        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view1 AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key IS NOT NULL AND v IS NOT NULL PRIMARY KEY (c, key, v) ")
        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view2 AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key IS NOT NULL AND v IS NOT NULL PRIMARY KEY (c, key, v) ")
        
        await wait_for_some_view_build_tasks_to_get_stuck(manager, marks)

        await cql.run_async(f"DROP MATERIALIZED VIEW {ks}.mv_cf_view2")

        await unpause_view_building_tasks(manager)

        await wait_for_view(cql, 'mv_cf_view1', node_count)
        await check_view_contents(cql, ks, "mv_cf_view1")


@pytest.mark.skip(reason = "done")
@pytest.mark.asyncio
async def test_alter_base_schema_while_build_in_progress(manager: ManagerClient):
    node_count = 4
    servers = await manager.servers_add(node_count)
    cql, _ = await manager.get_ready_cql(servers)
    async with new_test_keyspace(manager, f"WITH replication = {{'class': 'NetworkTopologyStrategy', 'replication_factor': 2}} AND tablets = {{'enabled': true}}") as ks:
        await cql.run_async(f"CREATE TABLE {ks}.tab (key int, c int, v text, PRIMARY KEY (key, c))")
        await populate_base_table(cql, ks, "tab")

        marks = await mark_all_servers(manager)
        await pause_view_building_tasks(manager)

        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key IS NOT NULL AND v IS NOT NULL PRIMARY KEY (c, key, v) ")
        
        await wait_for_some_view_build_tasks_to_get_stuck(manager, marks)

        await cql.run_async(f"ALTER TABLE {ks}.tab ADD u text")

        await unpause_view_building_tasks(manager)

        await wait_for_view(cql, 'mv_cf_view', node_count)
        await check_view_contents(cql, ks, "mv_cf_view")


@pytest.mark.skip(reason = "done")
@pytest.mark.asyncio
async def test_migrate_tablet_while_build_in_progress(manager: ManagerClient):
    node_count = 4
    servers = await manager.servers_add(node_count)
    cql, _ = await manager.get_ready_cql(servers)

    await disable_tablet_load_balancing_on_all_servers(manager)

    async with new_test_keyspace(manager, f"WITH replication = {{'class': 'NetworkTopologyStrategy', 'replication_factor': 2}} AND tablets = {{'enabled': true}}") as ks:
        await cql.run_async(f"CREATE TABLE {ks}.tab (key int, c int, v text, PRIMARY KEY (key, c))")
        await populate_base_table(cql, ks, "tab")

        token = 1
        (source_host, source_shard), (target_host, target_shard) = await get_viable_tablet_migration(manager, ks, "tab", token)

        marks = await mark_all_servers(manager)
        await pause_view_building_tasks(manager)

        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key IS NOT NULL AND v IS NOT NULL PRIMARY KEY (c, key, v) ")
        
        await wait_for_some_view_build_tasks_to_get_stuck(manager, marks)

        await manager.api.move_tablet(servers[0].ip_addr, ks, "tab", source_host, source_shard, target_host, target_shard, token, 60)

        await unpause_view_building_tasks(manager)

        await wait_for_view(cql, 'mv_cf_view', node_count)
        await check_view_contents(cql, ks, "mv_cf_view")


@pytest.mark.skip(reason = "todo")
@pytest.mark.asyncio
async def test_split_tablet_while_build_in_progress(manager: ManagerClient):
    node_count = 4
    servers = await manager.servers_add(node_count)
    cql, _ = await manager.get_ready_cql(servers)
    async with new_test_keyspace(manager, f"WITH replication = {{'class': 'NetworkTopologyStrategy', 'replication_factor': 2}} AND tablets = {{'enabled': true}}") as ks:
        await cql.run_async(f"CREATE TABLE {ks}.tab (key int, c int, v text, PRIMARY KEY (key, c))")
        await populate_base_table(cql, ks, "tab")

        # TODO: Enable injection that will pause the view build workers if they start executing tasks

        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key IS NOT NULL AND v IS NOT NULL PRIMARY KEY (c, key, v) ")
        
        # TODO: Wait for some workers to get stuck

        # TODO: Split a tablet

        # TODO: Unpause the workers

        await wait_for_view(cql, 'mv_cf_view', node_count)
        await check_view_contents(cql, ks, "mv_cf_view")


@pytest.mark.skip(reason = "todo")
@pytest.mark.asyncio
async def test_merge_two_tablets_before_being_built_while_build_in_progress(manager: ManagerClient):
    node_count = 4
    servers = await manager.servers_add(node_count)
    cql, _ = await manager.get_ready_cql(servers)
    async with new_test_keyspace(manager, f"WITH replication = {{'class': 'NetworkTopologyStrategy', 'replication_factor': 2}} AND tablets = {{'enabled': true}}") as ks:
        await cql.run_async(f"CREATE TABLE {ks}.tab (key int, c int, v text, PRIMARY KEY (key, c))")
        await populate_base_table(cql, ks, "tab")

        # TODO: Enable injection that will pause the view build workers if they start executing tasks

        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key IS NOT NULL AND v IS NOT NULL PRIMARY KEY (c, key, v) ")
        
        # TODO: Wait for some workers to get stuck

        # TODO: Merge two tablets

        # TODO: Unpause the workers

        await wait_for_view(cql, 'mv_cf_view', node_count)
        await check_view_contents(cql, ks, "mv_cf_view")


@pytest.mark.skip(reason = "todo")
@pytest.mark.asyncio
async def test_merge_two_tablets_after_one_is_built_while_build_in_progress(manager: ManagerClient):
    node_count = 4
    servers = await manager.servers_add(node_count)
    cql, _ = await manager.get_ready_cql(servers)
    async with new_test_keyspace(manager, f"WITH replication = {{'class': 'NetworkTopologyStrategy', 'replication_factor': 2}} AND tablets = {{'enabled': true}}") as ks:
        await cql.run_async(f"CREATE TABLE {ks}.tab (key int, c int, v text, PRIMARY KEY (key, c))")
        await populate_base_table(cql, ks, "tab")

        # TODO: Choose two tablets, block one from being processed by view build coordinator (can get stuck)
        # TODO: Enable injection that will pause the view build workers if they start executing tasks

        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key IS NOT NULL AND v IS NOT NULL PRIMARY KEY (c, key, v) ")
        
        # TODO: Wait until only one range build task remains in the table

        # TODO: Merge two tablets

        # TODO: Unpause the workers

        await wait_for_view(cql, 'mv_cf_view', node_count)
        await check_view_contents(cql, ks, "mv_cf_view")


@pytest.mark.skip(reason = "done")
@pytest.mark.asyncio
async def test_increase_rf_while_build_in_progress(manager: ManagerClient):
    node_count = 4
    servers = await manager.servers_add(node_count)
    cql, _ = await manager.get_ready_cql(servers)
    async with new_test_keyspace(manager, f"WITH replication = {{'class': 'NetworkTopologyStrategy', 'replication_factor': 1}} AND tablets = {{'enabled': true}}") as ks:
        await cql.run_async(f"CREATE TABLE {ks}.tab (key int, c int, v text, PRIMARY KEY (key, c))")
        await populate_base_table(cql, ks, "tab")

        marks = await mark_all_servers(manager)
        await pause_view_building_tasks(manager)

        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key IS NOT NULL AND v IS NOT NULL PRIMARY KEY (c, key, v) ")
        
        await wait_for_some_view_build_tasks_to_get_stuck(manager, marks)

        await cql.run_async(f"ALTER KEYSPACE {ks} WITH replication = {{'class': 'NetworkTopologyStrategy', 'datacenter1': 2}}")

        await unpause_view_building_tasks(manager)

        await wait_for_view(cql, 'mv_cf_view', node_count)
        await check_view_contents(cql, ks, "mv_cf_view")


@pytest.mark.skip(reason = "done")
@pytest.mark.asyncio
async def test_decrease_rf_while_build_in_progress(manager: ManagerClient):
    node_count = 4
    servers = await manager.servers_add(node_count)
    cql, _ = await manager.get_ready_cql(servers)
    async with new_test_keyspace(manager, f"WITH replication = {{'class': 'NetworkTopologyStrategy', 'replication_factor': 2}} AND tablets = {{'enabled': true}}") as ks:
        await cql.run_async(f"CREATE TABLE {ks}.tab (key int, c int, v text, PRIMARY KEY (key, c))")
        await populate_base_table(cql, ks, "tab")

        marks = await mark_all_servers(manager)
        await pause_view_building_tasks(manager)

        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key IS NOT NULL AND v IS NOT NULL PRIMARY KEY (c, key, v) ")
        
        await wait_for_some_view_build_tasks_to_get_stuck(manager, marks)

        await cql.run_async(f"ALTER KEYSPACE {ks} WITH replication = {{'class': 'NetworkTopologyStrategy', 'datacenter1': 1}}")

        await unpause_view_building_tasks(manager)

        await wait_for_view(cql, 'mv_cf_view', node_count)
        await check_view_contents(cql, ks, "mv_cf_view")


@pytest.mark.skip(reason = "todo")
@pytest.mark.asyncio
async def test_repair_while_build_in_progress(manager: ManagerClient):
    node_count = 4
    servers = await manager.servers_add(node_count)
    cql, _ = await manager.get_ready_cql(servers)
    async with new_test_keyspace(manager, f"WITH replication = {{'class': 'NetworkTopologyStrategy', 'replication_factor': 2}} AND tablets = {{'enabled': true}}") as ks:
        await cql.run_async(f"CREATE TABLE {ks}.tab (key int, c int, v text, PRIMARY KEY (key, c))")
        await populate_base_table(cql, ks, "tab")

        # TODO: Consider destroying some of the data, or disabling hints and populating the cluster with less consistency
        # This would require querying each node separately, though - maybe we need to adjust `check_view_contents` for this

        marks = await mark_all_servers(manager)
        await pause_view_building_tasks(manager)

        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key IS NOT NULL AND v IS NOT NULL PRIMARY KEY (c, key, v) ")
        
        await wait_for_some_view_build_tasks_to_get_stuck(manager, marks)

        await manager.api.repair(servers[0].ip_addr, ks, "tbl")

        await unpause_view_building_tasks(manager)

        await wait_for_view(cql, 'mv_cf_view', node_count)
        await check_view_contents(cql, ks, "mv_cf_view")


@pytest.mark.skip(reason = "todo")
@pytest.mark.asyncio
async def test_add_node_while_build_in_progress(manager: ManagerClient):
    node_count = 4
    servers = await manager.servers_add(node_count)
    cql, _ = await manager.get_ready_cql(servers)
    async with new_test_keyspace(manager, f"WITH replication = {{'class': 'NetworkTopologyStrategy', 'replication_factor': 2}} AND tablets = {{'enabled': true}}") as ks:
        await cql.run_async(f"CREATE TABLE {ks}.tab (key int, c int, v text, PRIMARY KEY (key, c))")
        await populate_base_table(cql, ks, "tab")

        marks = await mark_all_servers(manager)
        await pause_view_building_tasks(manager)

        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key IS NOT NULL AND v IS NOT NULL PRIMARY KEY (c, key, v) ")
        
        await wait_for_some_view_build_tasks_to_get_stuck(manager, marks)

        servers += [await manager.server_add()]
        node_count += 1

        await unpause_view_building_tasks(manager)

        # TODO: Probably we don't mark the newly added node as "done"?
        await wait_for_view(cql, 'mv_cf_view', node_count)
        await check_view_contents(cql, ks, "mv_cf_view")


@pytest.mark.skip(reason = "todo")
@pytest.mark.asyncio
async def test_remove_node_while_build_in_progress(manager: ManagerClient):
    node_count = 4
    servers = await manager.servers_add(node_count)
    cql, _ = await manager.get_ready_cql(servers)
    async with new_test_keyspace(manager, f"WITH replication = {{'class': 'NetworkTopologyStrategy', 'replication_factor': 2}} AND tablets = {{'enabled': true}}") as ks:
        await cql.run_async(f"CREATE TABLE {ks}.tab (key int, c int, v text, PRIMARY KEY (key, c))")
        await populate_base_table(cql, ks, "tab")

        marks = await mark_all_servers(manager)
        await pause_view_building_tasks(manager)

        await cql.run_async(f"CREATE MATERIALIZED VIEW {ks}.mv_cf_view AS SELECT * FROM {ks}.tab "
                        "WHERE c IS NOT NULL and key IS NOT NULL AND v IS NOT NULL PRIMARY KEY (c, key, v) ")
        
        # TODO: Need to make sure that the node being removed stops being stuck before being removed
        await wait_for_some_view_build_tasks_to_get_stuck(manager, marks)

        await manager.decommission_node(servers[-1].server_id)
        servers.pop()
        node_count -= 1

        await unpause_view_building_tasks(manager)

        await wait_for_view(cql, 'mv_cf_view', node_count)
        await check_view_contents(cql, ks, "mv_cf_view")

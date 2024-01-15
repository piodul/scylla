#
# Copyright (C) 2024-present ScyllaDB
#
# SPDX-License-Identifier: AGPL-3.0-or-later
#

import asyncio
import logging
import pytest
import requests
import time

from test.pylib.rest_client import HTTPError
from test.pylib.manager_client import ManagerClient
from test.pylib.util import wait_for_cql_and_get_hosts
from test.topology.util import log_run_time, wait_until_topology_upgrade_finishes, \
        check_system_topology_and_cdc_generations_v3_consistency


@pytest.mark.asyncio
@log_run_time
async def test_topology_upgrade_basic(request, manager: ManagerClient):
    # First, force the first node to start in legacy mode due to the error injection
    cfg = {'enable_user_defined_functions': False,
           'experimental_features': ['consistent-topology-changes'],
           'error_injections_at_startup': ['force_gossip_based_join']}
    
    servers = [await manager.server_add(config=cfg)]
    # Disable injections for the subsequent nodes - they should fall back to
    # using gossiper-based node operations
    del cfg['error_injections_at_startup']

    servers += [await manager.server_add(config=cfg) for _ in range(2)]
    cql = manager.cql
    assert(cql)

    logging.info("Waiting until driver connects to every server")
    hosts = await wait_for_cql_and_get_hosts(cql, servers, time.time() + 60)

    logging.info("Checking the upgrade state on all nodes")
    for host in hosts:
        status = await manager.api.raft_topology_upgrade_status(host.address)
        assert status == "not_upgraded"

    logging.info("Triggering upgrade to raft topology")
    await manager.api.upgrade_to_raft_topology(hosts[0].address)

    logging.info("Check that trying to trigger upgrade again fails")
    try:
        await manager.api.upgrade_to_raft_topology(hosts[0].address)
        pytest.fail("The second upgrade to raft topology command unexpectedly succeeded")
    except HTTPError as e:
        assert e.code == requests.codes.server_error

    logging.info("Waiting until upgrade finishes")
    await asyncio.gather(*(wait_until_topology_upgrade_finishes(manager, h.address, time.time() + 60) for h in hosts))

    logging.info("Checking consistency of data in system.topology and system.cdc_generations_v3")
    await asyncio.gather(*(check_system_topology_and_cdc_generations_v3_consistency(manager, h) for h in hosts))

    logging.info("Booting new node")
    await manager.server_add(config=cfg)

    logging.info("Waiting until driver connects to every server")
    hosts = await wait_for_cql_and_get_hosts(cql, servers, time.time() + 60)

    logging.info("Checking consistency of data in system.topology and system.cdc_generations_v3")
    await asyncio.gather(*(check_system_topology_and_cdc_generations_v3_consistency(manager, h) for h in hosts))

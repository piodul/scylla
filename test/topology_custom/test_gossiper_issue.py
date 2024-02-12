#
# Copyright (C) 2023-present ScyllaDB
#
# SPDX-License-Identifier: AGPL-3.0-or-later
#
import asyncio
import time
import pytest
import logging

from test.pylib.manager_client import ManagerClient
from test.pylib.util import wait_for_cql_and_get_hosts


SCHEMA_APPLICATION_STATE = 2


logger = logging.getLogger(__name__)


@pytest.mark.asyncio
async def test_boot_after_ip_change(manager: ManagerClient) -> None:
    """Bootstrap a new node after existing one changed its IP.
       Regression test for #14468. Does not apply to Raft-topology mode.
    """
    cfg = {'enable_user_defined_functions': False,
           'smp': 14,
           'experimental_features': list[str]()}
    logger.info(f"Booting initial cluster")
    servers = [await manager.server_add(config=cfg) for _ in range(12)]
    cql = manager.get_cql()
    hosts = await wait_for_cql_and_get_hosts(cql, servers, time.time() + 60)

    await cql.run_async("create keyspace ks with replication = {'class': 'SimpleStrategy', 'replication_factor': 1}")

    logger.info(f"Start blastin'")

    i = 0
    while True:
        i += 1
        # Change the schema
        logger.info(f"Test {i}")
        if i % 2 == 1:
            logger.info("Creating the table")
            await cql.run_async("create table ks.t (pk int PRIMARY KEY)")
        else:
            logger.info("Dropping the table")
            await cql.run_async("drop table ks.t")

        # Wait for schema agreement, and wait until nodes see schema agreement
        start_time = time.time()
        attempt = 0
        while True:
            attempt += 1
            elapsed = time.time() - start_time
            assert elapsed < 60.0
            logger.info(f"Checking schema agreement (test {i} / attempt {attempt} / elapsed {elapsed}s)")
            all_ok = True
            for host in hosts:
                ip = host.address
                data = await manager.api.client.get_json('/failure_detector/endpoints/', ip)
                app_states = {epdata['addrs']: next(x['value'] for x in epdata['application_state'] if x['application_state'] == SCHEMA_APPLICATION_STATE) for epdata in data}
                if len(set(app_states.values())) == 1:
                    logger.info(f"Schema OK on {host}")
                else:
                    logger.info(f"Schema not seen as in agreement on node {host}: {app_states}")
                    all_ok = False

            if all_ok:
                break
            else:
                await asyncio.sleep(0.5)

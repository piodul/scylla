# Copyright 2021-present ScyllaDB
#
# This file is part of Scylla.
#
# Scylla is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Scylla is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with Scylla.  If not, see <http://www.gnu.org/licenses/>.

from cassandra.cluster import ConsistencyLevel
from cassandra.query import SimpleStatement

from concurrent.futures import ThreadPoolExecutor
from threading import Event
from util import new_test_table
from nodetool import rest_api_url

import requests
import time


def run_concurrently(stop_event, concurrency, f):
    def work(i):
        try:
            f(i)
        except:
            stop_event.set()
            raise
    
    worker_executor = ThreadPoolExecutor(max_workers=concurrency)
    futs = [worker_executor.submit(work, i) for i in range(concurrency)]

    def joiner():
        worker_executor.shutdown(wait=True)
        for f in futs:
            f.result()
    return joiner


def test_switch_to_underlying_reader_on_flush(cql, test_keyspace):
    """Test that reads which happen during flush return correct result.
    The test runs many concurrent reads, runs a flush and makes sure that
    all reads returned correct data."""

    worker_count = 10
    row_count = 10 * 1000
    pk = 42

    v_value = "x" * 1000

    schema = "pk int, ck int, v text, primary key (pk, ck)"
    with new_test_table(cql, test_keyspace, schema) as table:
        # Load data concurrently
        stmt = cql.prepare(f"insert into {table} (pk, ck, v) values (?, ?, ?)")

        stop_event = Event()
        def write(worker_id):
            for i in range(worker_id, row_count, worker_count):
                if stop_event.is_set():
                    return
                cql.execute(stmt, (pk, i, v_value+str(i)))

        joiner = run_concurrently(stop_event, worker_count, write)
        joiner()

        # Run multiple concurrent reads
        stmt = cql.prepare(f"select ck, v from {table} where pk = {pk} order by ck desc")

        stop_event = Event()
        def read(worker_id):
            while not stop_event.is_set():
                result = cql.execute(stmt)
                actual_row_count = 0
                for i, (ck, v) in zip(range(row_count)[::-1], result):
                    assert ck == i, f"wrong ck: {ck} != {i}"
                    assert v == v_value+str(ck), f"wrong ck: {v} != {v_value+str(ck)}"
                    actual_row_count += 1
                assert actual_row_count == row_count, \
                    f"missing rows: {actual_row_count} != {row_count}"
        
        joiner = run_concurrently(stop_event, worker_count, read)

        try:
            time.sleep(0.5)
            # While reads are running, trigger a flush
            requests.post(f'{rest_api_url(cql)}/storage_service/keyspace_flush/{test_keyspace}', \
                params={'cf': table})
        finally:
            # Make sure that we abort the reads regardless of the result of the flush
            # and wait until everything is joined with
            stop_event.set()
            joiner()




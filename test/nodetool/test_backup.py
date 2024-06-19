#
# Copyright 2023-present ScyllaDB
#
# SPDX-License-Identifier: AGPL-3.0-or-later
#

from test.nodetool.rest_api_mock import expected_request


def test_disablebackup(nodetool):
    nodetool("disablebackup", expected_requests=[
        expected_request("POST", "/storage_service/incremental_backups", params={"value": "false"})])


def test_enablebackup(nodetool):
    nodetool("enablebackup", expected_requests=[
        expected_request("POST", "/storage_service/incremental_backups", params={"value": "true"})])


def test_statusbackup(nodetool):
    res = nodetool("statusbackup", expected_requests=[
        expected_request("GET", "/storage_service/incremental_backups", response=False)])
    assert res.stdout == "not running\n"

    res = nodetool("statusbackup", expected_requests=[
        expected_request("GET", "/storage_service/incremental_backups", response=True)])
    assert res.stdout == "running\n"

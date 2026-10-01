#!/usr/bin/python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Verify the binding API and its links in the generated driver overview."""
from html.parser import HTMLParser
from pathlib import Path
import sys
import unittest
import xml.etree.ElementTree as ET

BUILD = Path(sys.argv.pop(1)).resolve()


class Links(HTMLParser):
    def __init__(self, path):
        super().__init__()
        self.links = []
        self.feed(path.read_text())

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "a" and "href" in attrs:
            self.links.append(attrs["href"])


class DocumentationTests(unittest.TestCase):
    def test_binding_function_and_constant_have_public_pages(self):
        tree = ET.parse(BUILD / "pgsql.tag")
        for name in ("pgsql_bind", "PG_TYPE_CASH"):
            with self.subTest(name=name):
                members = [m for m in tree.findall(".//member") if m.findtext("name") == name]
                self.assertTrue(members, name)
                for member in members:
                    page = member.findtext("anchorfile")
                    self.assertTrue((BUILD / "docs/pgsql/html" / page).is_file(), page)

    def test_driver_overview_links_to_the_binding_function(self):
        tree = ET.parse(BUILD / "pgsql.tag")
        targets = {m.findtext("anchorfile") + "#" + m.findtext("anchor")
                   for m in tree.findall(".//member") if m.findtext("name") == "pgsql_bind"}
        links = Links(BUILD / "docs/pgsql/html/index.html").links
        self.assertTrue(targets.intersection(links), targets)

    def test_overview_links_to_sdk_timezone_and_bulk_reference(self):
        links = Links(BUILD / "docs/pgsql/html/index.html").links
        self.assertTrue(any(link.split("#", 1)[0].endswith("/time_zones.html") for link in links))
        self.assertTrue(any("/BulkSqlUtil/html/" in link for link in links))


if __name__ == "__main__":
    unittest.main()

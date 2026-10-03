#!/usr/bin/env python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Verify symbol links in built Git docs: python3 test/test_docs.py build -v."""

from html.parser import HTMLParser
from pathlib import Path
import re
import sys
import unittest
from urllib.parse import unquote, urlsplit
import xml.etree.ElementTree as ET


BUILD = Path(sys.argv.pop(1)).resolve()
HTML = BUILD / 'docs/git/html'
ROOT = Path(__file__).resolve().parents[1]


class Page(HTMLParser):
    def __init__(self, path):
        super().__init__()
        self.links = []
        self.ids = set()
        self.current = None
        self.feed(path.read_text())

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if 'id' in attrs:
            self.ids.add(attrs['id'])
        if tag == 'a' and 'href' in attrs:
            self.current = [attrs['href'], '']

    def handle_data(self, data):
        if self.current is not None:
            self.current[1] += data

    def handle_endtag(self, tag):
        if tag == 'a' and self.current is not None:
            self.links.append(tuple(self.current))
            self.current = None


class DocsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.page = Page(HTML / 'index.html')
        cls.tags = ET.parse(BUILD / 'git.tag')

    def symbol_target(self, compound_name, name):
        matches = [member for compound in self.tags.findall('compound')
                   if compound.findtext('name') == compound_name
                   for member in compound.findall('member') if member.findtext('name') == name]
        self.assertEqual(1, len(matches), (compound_name, name))
        member = matches[0]
        return member.findtext('anchorfile') + '#' + member.findtext('anchor')

    def assert_local_target(self, href):
        url = urlsplit(href)
        self.assertFalse(url.scheme or url.netloc, href)
        path = HTML / unquote(url.path or 'index.html')
        self.assertTrue(path.is_file(), href)
        if url.fragment:
            self.assertIn(unquote(url.fragment), Page(path).ids, href)

    def test_status_method_links_to_api(self):
        target = self.symbol_target('Qore::Git::GitRepository', 'status')
        self.assertIn((target, 'GitRepository::status()'), self.page.links)
        self.assert_local_target(target)

    def test_all_status_flags_link_to_api_and_keep_old_anchors(self):
        names = re.findall(r'^const (GIT_STATUS_\w+) =', (ROOT / 'src/ql_git.qpp').read_text(), re.M)
        self.assertEqual(11, len(names))
        for name in names:
            with self.subTest(name=name):
                target = self.symbol_target('Qore::Git', name)
                self.assertNotEqual('index.html', urlsplit(target).path)
                self.assertIn((target, name), self.page.links)
                self.assertIn(name, self.page.ids)
                self.assert_local_target(target)

    def test_all_exported_constants_have_api_targets(self):
        names = re.findall(r'^const (GIT_\w+) =', (ROOT / 'src/ql_git.qpp').read_text(), re.M)
        self.assertEqual(25, len(names))
        for name in names:
            with self.subTest(name=name):
                self.assert_local_target(self.symbol_target('Qore::Git', name))

    def test_module_links_use_intro_targets_and_class_link_resolves(self):
        for name in ('GitDataProvider', 'GitConnections', 'GitConfigManager', 'GitForgeHelper',
                     'ConnectionProvider', 'DataProvider'):
            links = [href for href, label in self.page.links if label == name]
            self.assertTrue(links, name)
            for href in links:
                self.assertEqual(name.lower() + 'intro', urlsplit(href).fragment)
        links = [href for href, label in self.page.links if label == 'AbstractWatchDataProviderBase']
        self.assertEqual(1, len(links))
        # The framework class is imported from the SDK's selected DataProvider index.
        tagfile, destination = re.search(r'"([^"=]+/DataProvider\.tag)=([^"\n]+)"',
                                         (BUILD / 'Doxyfile').read_text()).groups()
        targets = [compound.findtext('filename') for compound in ET.parse(tagfile).findall('compound')
                   if compound.findtext('name') == 'DataProvider::AbstractWatchDataProviderBase']
        self.assertEqual(1, len(targets))
        self.assertEqual(destination + '/' + targets[0], links[0])

    def test_mainpage_local_links_and_fragments_exist(self):
        for href, _ in self.page.links:
            url = urlsplit(href)
            if not url.scheme and not url.netloc:
                with self.subTest(href=href):
                    self.assert_local_target(href)


if __name__ == '__main__':
    unittest.main()

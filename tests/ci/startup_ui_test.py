"""Release UI 回归识别标准可访问名称，并拒绝其他应用的同名节点。"""
from pathlib import Path
import sys
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/packaging"))
from sample_release_startup_test import PACKAGE, node_has_label, visible_labels


class StartupUiTest(unittest.TestCase):
    def test_text_and_accessibility_description_are_names(self):
        tree = ET.fromstring(f'<hierarchy><node package="{PACKAGE}" text="拾流"/>'
                             f'<node package="{PACKAGE}" content-desc="新建下载"/></hierarchy>')
        self.assertTrue({"拾流", "新建下载"} <= visible_labels(tree))
        self.assertTrue(node_has_label(list(tree)[1], "新建下载"))

    def test_foreign_or_unnamed_nodes_cannot_satisfy_the_check(self):
        tree = ET.fromstring(f'<hierarchy><node package="other.app" text="新建下载"/>'
                             f'<node package="{PACKAGE}" text=""/></hierarchy>')
        self.assertNotIn("新建下载", visible_labels(tree))
        self.assertFalse(node_has_label(list(tree)[0], "新建下载"))
        self.assertFalse(node_has_label(list(tree)[1], "新建下载"))


if __name__ == "__main__":
    unittest.main()

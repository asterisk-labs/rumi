from tools.check_release import r_package_version

CHANGELOG = """\
## [Unreleased]

## [0.26.1] - 2026-09-29

## [0.26.0] - 2026-09-27
"""


def test_r_package_version():
    assert r_package_version("0.27.0", CHANGELOG) == "0.27.0"
    assert r_package_version("0.27.0-alpha.1", CHANGELOG) == "0.26.1.9001"
    assert r_package_version("0.27.0-beta.2", CHANGELOG) == "0.26.1.9102"
    assert r_package_version("0.27.0-rc.3", CHANGELOG) == "0.26.1.9203"

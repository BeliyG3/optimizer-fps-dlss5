"""Prefixed console logger shared by the bench Blender export scripts."""


def make_log(prefix):
    """Return ``log(*a)`` that prints ``prefix`` first, e.g. ``make_log("[cutegirl]")``."""
    def log(*a):
        print(prefix, *a)
    return log
